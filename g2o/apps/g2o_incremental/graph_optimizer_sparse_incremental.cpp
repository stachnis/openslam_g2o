#include "graph_optimizer_sparse_incremental.h"

#include "g2o/apps/g2o_interactive/types_slam2d_online.h"
#include "g2o/apps/g2o_interactive/types_slam3d_online.h"

#include "g2o/core/block_solver.h"
#include "g2o/stuff/macros.h"

#define DIM_TO_SOLVER(p, l) BlockSolver< BlockSolverTraits<p, l> >

#define ALLOC_CHOLMOD(s, p, l) \
  if (1) { \
    std::cerr << "# Using CHOLMOD online poseDim " << p << " landMarkDim " << l << " blockordering 1" << std::endl; \
    LinearSolverCholmodOnline < DIM_TO_SOLVER(p, l)::PoseMatrixType >* linearSolver = new LinearSolverCholmodOnline<DIM_TO_SOLVER(p, l)::PoseMatrixType>(); \
    s = new DIM_TO_SOLVER(p, l)(opt, linearSolver); \
  } else (void)0

using namespace std;

namespace g2o {

  namespace {
    struct VertexBackup
    {
      int tempIndex;
      OptimizableGraph::Vertex* vertex;
      double* hessianData;
      bool operator<(const VertexBackup& other) const
      {
        return tempIndex < other.tempIndex;
      }
    };
  }

  SparseOptimizerIncremental::SparseOptimizerIncremental()
  {
    _cholmodSparse = new CholmodExt();
    _cholmodFactor = 0;
    cholmod_start(&_cholmodCommon);

    // setup ordering strategy
    _cholmodCommon.nmethods = 1 ;
    _cholmodCommon.method[0].ordering = CHOLMOD_GIVEN;
    //_cholmodCommon.postorder = 0;
    _cholmodCommon.supernodal = CHOLMOD_SIMPLICIAL;

    _permutedUpdate = cholmod_allocate_triplet(1000, 1000, 1024, 0, CHOLMOD_REAL, &_cholmodCommon);
    _L = 0;
    _cholmodFactor = 0;
  }

  SparseOptimizerIncremental::~SparseOptimizerIncremental()
  {
    _updateMat.clear(true);
    delete _cholmodSparse;
    if (_cholmodFactor) {
      cholmod_free_factor(&_cholmodFactor, &_cholmodCommon);
      _cholmodFactor = 0;
    }
    cholmod_free_triplet(&_permutedUpdate, &_cholmodCommon);
    cholmod_finish(&_cholmodCommon);
  }

  int SparseOptimizerIncremental::optimize(int iterations, bool online)
  {
    //return SparseOptimizer::optimize(iterations, online);

    (void) iterations; // we only do one iteration anyhow
    Solver* solver = _solver;
    solver->init(online);

    int cjIterations=0;
    bool ok=true;

    if (! online || batchStep) {
      //cerr << "performing batch step" << endl;
      if (! online) {
        ok = solver->buildStructure();
        if (! ok) {
          cerr << __PRETTY_FUNCTION__ << ": Failure while building CCS structure" << endl;
          return 0;
        }
      }

      // copy over the updated estimate as new linearization point
      if (slamDimension == 3) {
        for (size_t i = 0; i < indexMapping().size(); ++i) {
          OnlineVertexSE2* v = static_cast<OnlineVertexSE2*>(indexMapping()[i]);
          v->estimate() = v->updatedEstimate;
        }
      }
      else if (slamDimension == 6) {
        for (size_t i = 0; i < indexMapping().size(); ++i) {
          OnlineVertexSE3* v= static_cast<OnlineVertexSE3*>(indexMapping()[i]);
          v->estimate() = v->updatedEstimate;
        }
      }

      SparseOptimizer::computeActiveErrors();
      SparseOptimizer::linearizeSystem();
      solver->buildSystem();

      int numBlocksRequired = _ivMap.size();
      if (_cmember.size() < numBlocksRequired) {
        _cmember.resize(2 * numBlocksRequired);
      }
      memset(_cmember.data(), 0, numBlocksRequired * sizeof(int));
      if (_ivMap.size() > 100) {
        for (size_t i = _ivMap.size() - 20; i < _ivMap.size(); ++i) {
          const HyperGraph::EdgeSet& eset = _ivMap[i]->edges();
          for (HyperGraph::EdgeSet::const_iterator it = eset.begin(); it != eset.end(); ++it) {
            OptimizableGraph::Edge* e = static_cast<OptimizableGraph::Edge*>(*it);
            OptimizableGraph::Vertex* v1 = static_cast<OptimizableGraph::Vertex*>(e->vertices()[0]);
            OptimizableGraph::Vertex* v2 = static_cast<OptimizableGraph::Vertex*>(e->vertices()[1]);
            if (v1->tempIndex() >= 0)
              _cmember(v1->tempIndex()) = 1;
            if (v2->tempIndex() >= 0)
              _cmember(v2->tempIndex()) = 1;
          }
        }
      }

      ok = solver->solve();

      // get the current cholesky factor
      if (slamDimension == 3) {
        BlockSolver<BlockSolverTraits<3, 2> >* bs = dynamic_cast<BlockSolver<BlockSolverTraits<3, 2> >*>(solver);
        LinearSolverCholmodOnline<Matrix3d>* s = dynamic_cast<LinearSolverCholmodOnline<Matrix3d>*>(bs->linearSolver());
        _L = s->L();
      }
      else if (slamDimension == 6) {
        BlockSolver<BlockSolverTraits<6, 3> >* bs = dynamic_cast<BlockSolver<BlockSolverTraits<6, 3> >*>(solver);
        LinearSolverCholmodOnline<Matrix<double, 6, 6> >* s = dynamic_cast<LinearSolverCholmodOnline<Matrix<double, 6, 6> >*>(bs->linearSolver());
        _L = s->L();
      }
      if (_perm.size() < (int)_L->n)
        _perm.resize(2*_L->n);
      int* p = (int*)_L->Perm;
      for (size_t i = 0; i < _L->n; ++i)
        _perm[p[i]] = i;

    }
    else {
      // update the b vector
      for (HyperGraph::VertexSet::iterator it = _touchedVertices.begin(); it != _touchedVertices.end(); ++it) {
        OptimizableGraph::Vertex* v = static_cast<OptimizableGraph::Vertex*>(*it);
        int iBase = v->colInHessian();
        v->copyB(solver->b() + iBase);
      }
      if (slamDimension == 3) {
        BlockSolver<BlockSolverTraits<3, 2> >* bs = static_cast<BlockSolver<BlockSolverTraits<3, 2> >*>(solver);
        LinearSolverCholmodOnline<Matrix3d>* s = static_cast<LinearSolverCholmodOnline<Matrix3d>*>(bs->linearSolver());
        s->solve(solver->x(), solver->b());
      }
      else if (slamDimension == 6) {
        BlockSolver<BlockSolverTraits<6, 3> >* bs = static_cast<BlockSolver<BlockSolverTraits<6, 3> >*>(solver);
        LinearSolverCholmodOnline<Matrix<double, 6, 6> >* s = static_cast<LinearSolverCholmodOnline<Matrix<double, 6, 6> >*>(bs->linearSolver());
        s->solve(solver->x(), solver->b());
      }
    }

    update(solver->x());
    ++cjIterations; 

    if (verbose()){
      computeActiveErrors();
      cerr
        << "nodes = " << vertices().size()
        << "\t edges= " << _activeEdges.size()
        << "\t chi2= " << FIXED(activeChi2())
        << endl << endl;
    }

    if (vizWithGnuplot)
      gnuplotVisualization();

    if (! ok)
      return 0;
    return 1;
  }

  bool SparseOptimizerIncremental::updateInitialization(HyperGraph::VertexSet& vset, HyperGraph::EdgeSet& eset)
  {
    if (batchStep) {
      return SparseOptimizerOnline::updateInitialization(vset, eset);
    }

    //cerr << __PRETTY_FUNCTION__ << endl;

    for (HyperGraph::VertexSet::iterator it = vset.begin(); it != vset.end(); ++it) {
      OptimizableGraph::Vertex* v = static_cast<OptimizableGraph::Vertex*>(*it);
      v->clearQuadraticForm(); // be sure that b is zero for this vertex
    }

    // get the touched vertices
    _touchedVertices.clear();
    for (HyperGraph::EdgeSet::iterator it = eset.begin(); it != eset.end(); ++it) {
      OptimizableGraph::Edge* e = static_cast<OptimizableGraph::Edge*>(*it);
      OptimizableGraph::Vertex* v1 = static_cast<OptimizableGraph::Vertex*>(e->vertices()[0]);
      OptimizableGraph::Vertex* v2 = static_cast<OptimizableGraph::Vertex*>(e->vertices()[1]);
      if (! v1->fixed())
        _touchedVertices.insert(v1);
      if (! v2->fixed())
        _touchedVertices.insert(v2);
    }
    //cerr << PVAR(_touchedVertices.size()) << endl;

    // updating the internal structures
    std::vector<HyperGraph::Vertex*> newVertices;
    newVertices.reserve(vset.size());
    _activeVertices.reserve(_activeVertices.size() + vset.size());
    _activeEdges.reserve(_activeEdges.size() + eset.size());
    for (HyperGraph::EdgeSet::iterator it = eset.begin(); it != eset.end(); ++it)
      _activeEdges.push_back(static_cast<OptimizableGraph::Edge*>(*it));
    //cerr << "updating internal done." << endl;

    // update the index mapping
    size_t next = _ivMap.size();
    for (HyperGraph::VertexSet::iterator it = vset.begin(); it != vset.end(); ++it) {
      OptimizableGraph::Vertex* v=static_cast<OptimizableGraph::Vertex*>(*it);
      if (! v->fixed()){
        if (! v->marginalized()){
          v->setTempIndex(next);
          _ivMap.push_back(v);
          newVertices.push_back(v);
          _activeVertices.push_back(v);
          next++;
        } 
        else // not supported right now
          abort();
      }
      else {
        v->setTempIndex(-1);
      }
    }
    //cerr << "updating index mapping done." << endl;

    // backup the tempindex and prepare sorting structure
    VertexBackup backupIdx[_touchedVertices.size()];
    memset(backupIdx, 0, sizeof(VertexBackup) * _touchedVertices.size());
    int idx = 0;
    for (HyperGraph::VertexSet::iterator it = _touchedVertices.begin(); it != _touchedVertices.end(); ++it) {
      OptimizableGraph::Vertex* v = static_cast<OptimizableGraph::Vertex*>(*it);
      backupIdx[idx].tempIndex = v->tempIndex();
      backupIdx[idx].vertex = v;
      backupIdx[idx].hessianData = v->hessianData();
      ++idx;
    }
    sort(backupIdx, backupIdx + _touchedVertices.size()); // sort according to the tempIndex which is the same order as used later by the optimizer
    for (int i = 0; i < idx; ++i) {
      backupIdx[i].vertex->setTempIndex(i);
    }
    //cerr << "backup tempindex done." << endl;

    // building the structure of the update
    _updateMat.clear(true); // get rid of the old matrix structure
    _updateMat.rowBlockIndices().clear();
    _updateMat.colBlockIndices().clear();
    _updateMat.blockCols().clear();

    // placing the current stuff in _updateMat
    MatrixXd* lastBlock = 0;
    int sizePoses = 0;
    for (int i = 0; i < idx; ++i) {
      OptimizableGraph::Vertex* v = backupIdx[i].vertex;
      int dim = v->dimension();
      sizePoses+=dim;
      _updateMat.rowBlockIndices().push_back(sizePoses);
      _updateMat.colBlockIndices().push_back(sizePoses);
      _updateMat.blockCols().push_back(SparseBlockMatrix<MatrixXd>::IntBlockMap());
      int ind = v->tempIndex();
      //cerr << PVAR(ind) << endl;
      if (ind >= 0) {
        MatrixXd* m = _updateMat.block(ind, ind, true);
        v->mapHessianMemory(m->data());
        lastBlock = m;
      }
    }
    lastBlock->diagonal().array() += 1e-6; // HACK to get Eigen value > 0


    for (HyperGraph::EdgeSet::const_iterator it = eset.begin(); it != eset.end(); ++it) {
      OptimizableGraph::Edge* e = static_cast<OptimizableGraph::Edge*>(*it);
      OptimizableGraph::Vertex* v1 = (OptimizableGraph::Vertex*) e->vertices()[0];
      OptimizableGraph::Vertex* v2 = (OptimizableGraph::Vertex*) e->vertices()[1];

      int ind1 = v1->tempIndex();
      if (ind1 == -1)
        continue;
      int ind2 = v2->tempIndex();
      if (ind2 == -1)
        continue;
      bool transposedBlock = ind1 > ind2;
      if (transposedBlock) // make sure, we allocate the upper triangular block
        swap(ind1, ind2);

      MatrixXd* m = _updateMat.block(ind1, ind2, true);
      e->mapHessianMemory(m->data(), 0, 1, transposedBlock);
    }

    // build the system into _updateMat
    for (HyperGraph::EdgeSet::iterator it = eset.begin(); it != eset.end(); ++it) {
      OptimizableGraph::Edge * e = static_cast<OptimizableGraph::Edge*>(*it);
      e->computeError();
    }
    for (HyperGraph::EdgeSet::iterator it = eset.begin(); it != eset.end(); ++it) {
      OptimizableGraph::Edge* e = static_cast<OptimizableGraph::Edge*>(*it);
      e->linearizeOplus();
    }
    for (HyperGraph::EdgeSet::iterator it = eset.begin(); it != eset.end(); ++it) {
      OptimizableGraph::Edge* e = static_cast<OptimizableGraph::Edge*>(*it);
      e->constructQuadraticForm();
    }

    // restore the original data for the vertex
    for (int i = 0; i < idx; ++i) {
      backupIdx[i].vertex->setTempIndex(backupIdx[i].tempIndex);
      if (backupIdx[i].hessianData)
        backupIdx[i].vertex->mapHessianMemory(backupIdx[i].hessianData);
    }

    // update the structure of the real block matrix
    bool solverStatus = _solver->updateStructure(newVertices, eset);

    bool updateStatus = computeCholeskyUpdate();
    if (! updateStatus) {
      cerr << "Error while computing update" << endl;
    }

    cholmod_sparse* updateAsSparseFactor = cholmod_factor_to_sparse(_cholmodFactor, &_cholmodCommon);

    // convert CCS update by permuting back to the permutation of L
    if (updateAsSparseFactor->nzmax > _permutedUpdate->nzmax) {
      //cerr << "realloc _permutedUpdate" << endl;
      cholmod_reallocate_triplet(updateAsSparseFactor->nzmax, _permutedUpdate, &_cholmodCommon);
    }
    _permutedUpdate->nnz = 0;
    _permutedUpdate->nrow = _permutedUpdate->ncol = _L->n;
    {
      int* Ap = (int*)updateAsSparseFactor->p;
      int* Ai = (int*)updateAsSparseFactor->i;
      double* Ax = (double*)updateAsSparseFactor->x;
      int* Bj = (int*)_permutedUpdate->j;
      int* Bi = (int*)_permutedUpdate->i;
      double* Bx = (double*)_permutedUpdate->x;
      for (size_t c = 0; c < updateAsSparseFactor->ncol; ++c) {
        const int& rbeg = Ap[c];
        const int& rend = Ap[c+1];
        int cc = c / slamDimension;
        int coff = c % slamDimension;
        const int& cbase = backupIdx[cc].vertex->colInHessian();
        const int& ccol = _perm(cbase + coff);
        for (int j = rbeg; j < rend; j++) {
          const int& r = Ai[j];
          const double& val = Ax[j];

          int rr = r / slamDimension;
          int roff = r % slamDimension;
          const int& rbase = backupIdx[rr].vertex->colInHessian();
          
          int row = _perm(rbase + roff);
          int col = ccol;
          if (col > row) // lower triangular entry
            swap(col, row);
          Bi[_permutedUpdate->nnz] = row;
          Bj[_permutedUpdate->nnz] = col;
          Bx[_permutedUpdate->nnz] = val;
          ++_permutedUpdate->nnz;
        }
      }
    }
    cholmod_free_sparse(&updateAsSparseFactor, &_cholmodCommon);
    cholmod_sparse* updatePermuted = cholmod_triplet_to_sparse(_permutedUpdate, _permutedUpdate->nnz, &_cholmodCommon);
    //writeCCSMatrix("update-permuted.txt", updatePermuted->nrow, updatePermuted->ncol, (int*)updatePermuted->p, (int*)updatePermuted->i, (double*)updatePermuted2->x, false);

    if (slamDimension == 3) {
      BlockSolver<BlockSolverTraits<3, 2> >* bs = dynamic_cast<BlockSolver<BlockSolverTraits<3, 2> >*>(solver());
      LinearSolverCholmodOnline<Matrix3d>* s = dynamic_cast<LinearSolverCholmodOnline<Matrix3d>*>(bs->linearSolver());
      s->choleskyUpdate(updatePermuted);
    }
    else if (slamDimension == 6) {
      BlockSolver<BlockSolverTraits<6, 3> >* bs = dynamic_cast<BlockSolver<BlockSolverTraits<6, 3> >*>(solver());
      LinearSolverCholmodOnline<Matrix<double, 6, 6> >* s = dynamic_cast<LinearSolverCholmodOnline<Matrix<double, 6, 6> >*>(bs->linearSolver());
      s->choleskyUpdate(updatePermuted);
    }

    cholmod_free_sparse(&updatePermuted, &_cholmodCommon);

    return solverStatus;
  }

  bool SparseOptimizerIncremental::computeCholeskyUpdate()
  {
    if (_cholmodFactor) {
      cholmod_free_factor(&_cholmodFactor, &_cholmodCommon);
      _cholmodFactor = 0;
    }

    const SparseBlockMatrix<MatrixXd>& A = _updateMat;
    size_t m = A.rows();
    size_t n = A.cols();

    if (_cholmodSparse->columnsAllocated < n) {
      //std::cerr << __PRETTY_FUNCTION__ << ": reallocating columns" << std::endl;
      _cholmodSparse->columnsAllocated = _cholmodSparse->columnsAllocated == 0 ? n : 2 * n; // pre-allocate more space if re-allocating
      delete[] (int*)_cholmodSparse->p;
      _cholmodSparse->p = new int[_cholmodSparse->columnsAllocated+1];
    }
    size_t nzmax = A.nonZeros();
    if (_cholmodSparse->nzmax < nzmax) {
      //std::cerr << __PRETTY_FUNCTION__ << ": reallocating row + values" << std::endl;
      _cholmodSparse->nzmax = _cholmodSparse->nzmax == 0 ? nzmax : 2 * nzmax; // pre-allocate more space if re-allocating
      delete[] (double*)_cholmodSparse->x;
      delete[] (int*)_cholmodSparse->i;
      _cholmodSparse->i = new int[_cholmodSparse->nzmax];
      _cholmodSparse->x = new double[_cholmodSparse->nzmax];
    }
    _cholmodSparse->ncol = n;
    _cholmodSparse->nrow = m;

    A.fillCCS((int*)_cholmodSparse->p, (int*)_cholmodSparse->i, (double*)_cholmodSparse->x, true);
    //writeCCSMatrix("updatesparse.txt", _cholmodSparse->nrow, _cholmodSparse->ncol, (int*)_cholmodSparse->p, (int*)_cholmodSparse->i, (double*)_cholmodSparse->x, true);

    _cholmodFactor = cholmod_analyze(_cholmodSparse, &_cholmodCommon);
    cholmod_factorize(_cholmodSparse, _cholmodFactor, &_cholmodCommon);

    if (_cholmodCommon.status == CHOLMOD_NOT_POSDEF) {
      //std::cerr << "Cholesky failure, writing debug.txt (Hessian loadable by Octave)" << std::endl;
      //writeCCSMatrix("debug.txt", _cholmodSparse->nrow, _cholmodSparse->ncol, (int*)_cholmodSparse->p, (int*)_cholmodSparse->i, (double*)_cholmodSparse->x, true);
      return false;
    }

    // change to the specific format we need to have a pretty normal L
    int change_status = cholmod_change_factor(CHOLMOD_REAL, 1, 0, 1, 1, _cholmodFactor, &_cholmodCommon);
    if (! change_status) {
      return false;
    }

    return true;
  }

  static Solver* createSolver(SparseOptimizer* opt, const std::string& solverName)
  {
    g2o::Solver* s = 0;

    if (solverName == "fix3_2_cholmod") {
      ALLOC_CHOLMOD(s, 3, 2);
      s->setAdditionalVectorSpace(300);
    }
    else if (solverName == "fix6_3_cholmod") {
      ALLOC_CHOLMOD(s, 6, 3);
      s->setAdditionalVectorSpace(600);
    }

    return s;
  }

  bool SparseOptimizerIncremental::initSolver(int dimension, int batchEveryN)
  {
    cerr << __PRETTY_FUNCTION__ << endl;
    slamDimension = dimension;
    if (dimension == 3) {
      setSolver(createSolver(this, "fix3_2_cholmod"));
      BlockSolver<BlockSolverTraits<3, 2> >* bs = dynamic_cast<BlockSolver<BlockSolverTraits<3, 2> >*>(solver());
      LinearSolverCholmodOnline<Matrix3d>* s = dynamic_cast<LinearSolverCholmodOnline<Matrix3d>*>(bs->linearSolver());
      s->cmember = &_cmember;
      s->batchEveryN = batchEveryN;
    } else {
      setSolver(createSolver(this, "fix6_3_cholmod"));
      BlockSolver<BlockSolverTraits<6, 3> >* bs = dynamic_cast<BlockSolver<BlockSolverTraits<6, 3> >*>(solver());
      LinearSolverCholmodOnline<Matrix<double, 6, 6> >* s = dynamic_cast<LinearSolverCholmodOnline<Matrix<double, 6, 6> >*>(bs->linearSolver());
      s->cmember = &_cmember;
      s->batchEveryN = batchEveryN;
    }
    solver()->setSchur(false);
    if (! solver()) {
      cerr << "Error allocating solver. Allocating CHOLMOD solver failed!" << endl;
      return false;
    }
    return true;
  }

} // end namespace
