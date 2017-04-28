#ifndef traversal_h
#define traversal_h

int    nspawn;                                                  //!< Threshold of NBODY for spawning new threads
real_t theta;                                                   //!< Multipole acceptance criterion

void splitCell(Cell * Ci, Cell * Cj);

struct Traversal {                                              //!< Functor for traversal
  Cell * Ci;                                                    //!< Target cell pointer
  Cell * Cj;                                                    //!< Source cell pointer
  Traversal(Cell * _Ci, Cell * _Cj) : Ci(_Ci), Cj(_Cj) {};      // Constructor
  void operator() () const {                                          // Functor
    real_t dX[2];                                               // Distance vector
    for (int d=0; d<2; d++) dX[d] = Ci->X[d] - Cj->X[d];        // Distance vector from source to target
    real_t R2 = (dX[0] * dX[0] + dX[1] * dX[1]) * theta * theta;// Scalar distance squared
    if (R2 > (Ci->R + Cj->R) * (Ci->R + Cj->R)) {               // If distance is far enough
#ifdef LIST
      Ci->M2L.push_back(Cj);                                    //  Queue M2L kernels
#else
      M2L(Ci, Cj);                                              //  Use approximate kernels
#endif
    } else if (Ci->NNODE == 1 && Cj->NNODE == 1) {              // Else if both cells are bodies
#ifdef LIST
      Ci->P2P.push_back(Cj);                                    //  Queue P2P kernels
#else
      P2P(Ci, Cj);                                              //  Use exact kernel
#endif
    } else {                                                    // Else if cells are close but not bodies
      splitCell(Ci, Cj);                                        //  Split cell and call function recursively for child
    }                                                           // End if for multipole acceptance
  }                                                             // End functor
};

//! Split cell and call dualTreeTraversal() recursively for child
void splitCell(Cell * Ci, Cell * Cj) {
  tbb::task_group tg;                                           // Create task group
  if (Cj->NNODE == 1) {                                         // If Cj is leaf
    for (int i=0; i<4; i++) {                                   //  Loop over Ci's children
      if (Ci->CHILD[i]) {                                       //  If child exists
        Traversal traversal(Ci->CHILD[i], Cj);                  //   Instantiate recursive functor
        if (Ci->CHILD[i]->NBODY > nspawn) tg.run(traversal);    //   If task is large, spawn new task
        else traversal();                                       //   Else traverse on old task
      }                                                         //  End if child exists
    }                                                           //  End loop over Ci's children
  } else if (Ci->NNODE == 1) {                                  // Else if Ci is leaf
    for (int i=0; i<4; i++) {                                   //  Loop over Cj's children
      if (Cj->CHILD[i]) {                                       //  If child exists
        Traversal traversal(Ci, Cj->CHILD[i]);                  //   Instantiate recursive functor
        traversal();                                            //   Traverse on old thread
      }                                                         //  End if child exists
    }                                                           //  End loop over Cj's children
  } else if (Ci->R >= Cj->R) {                                  // Else if Ci is larger than Cj
    for (int i=0; i<4; i++) {                                   //  Loop over Ci's children
      if (Ci->CHILD[i]) {                                       //  If child exists
        Traversal traversal(Ci->CHILD[i], Cj);                  //   Instantiate recursive functor
        if (Ci->CHILD[i]->NBODY > nspawn) tg.run(traversal);    //   If task is large, spawn new task
        else traversal();                                       //   Else traverse on old task
      }                                                         //  End if child exists
    }                                                           //  End loop over Ci's children
  } else {                                                      // Else if Cj is larger than Ci
    for (int i=0; i<4; i++) {                                   //  Loop over Cj's children
      if (Cj->CHILD[i]) {                                       //  If child exists
        Traversal traversal(Ci, Cj->CHILD[i]);                  //   Instantiate recursive functor
        traversal();                                            //   Traverse on old thread 
      }                                                         //  End if child exists
    }                                                           //  End loop over Cj's children
  }                                                             // End if for leafs and Ci Cj size
  tg.wait();                                                    // Sync threads
}


//! Recursive call for upward pass 
void upwardPass(Cell * C) {
  for (int i=0; i<4; i++) {                                     // Loop over child cells
    if (C->CHILD[i]) upwardPass(C->CHILD[i]);                   //  Recursive call with new task
  }                                                             // End loop over child cells
  for (int n=0; n<P; n++) C->M[n] = 0;                          // Initialize multipole expansion coefficients
  for (int n=0; n<P; n++) C->L[n] = 0;                          // Initialize local expansion coefficients
  if (C->NNODE == 1) P2M(C);                                    // P2M kernel
  M2M(C);                                                       // M2M kernel
}

#ifdef LIST
#include "evaluate.h"

#ifdef TAO
#include "fmm-tao.h"
#endif

void breadthFirst(Cell *C) {
  std::queue<Cell*> cellQueue;                                  // Queue of cells for breadth first traversal
  cellQueue.push(C);                                            // Push root into queue
#ifdef TAO
  int ndx = 0, ins = 0;
  int NA = 10 * (numWorkers / awidth);
  fmm_st  *fmms[NA];                                           // create a set of fmm_st assemblies
  std::cout << "numWorkers: " << numWorkers <<", awidth: " << awidth << std::endl;
  gotao_init_hw(numWorkers,-1,-1);
  for(int i = 0; i < NA; i++){
    fmms[i] = new fmm_st(awidth);
    if(!fmms[i]) std::cout << "Initialization failed\n";
#ifdef TOPOPLACES
    fmms[i]->set_place(((float)  i) / (( float) NA));
#else
    fmms[i]->set_place(0.0);
#endif
  //  std::cout << "affinity queue " << fmms[i]->affinity_queue << std::endl;
    }

  fmms[ndx]->insert(C); 
#else
  std::vector<Cell*> cellVector;                                // Vector of cells
  cellVector.push_back(C);                                      // Push root into vector
  // I delay this in the hope of not interfering with the DTT, which is based on TBB
#endif
  while (!cellQueue.empty()) {                                  // While queue is not empty
    C = cellQueue.front();                                      //  Read front
    cellQueue.pop();                                            //  Pop queue
    for (int i=0; i<4; i++) {                                   //  Loop over child cells
      if (C->CHILD[i]) {                                        //   If child exists
#ifdef TAO
        fmms[ndx]->insert(C->CHILD[i]); 
	ins++; 
	if(ins == (awidth*awidth*AMULT)){
		ins = 0; 
		ndx = (ndx + 1) % NA;
		}
#else
        cellVector.push_back(C->CHILD[i]);                      //    Push child to vector
#endif
        cellQueue.push(C->CHILD[i]);                            //    Push child to queue
      }                                                         //   End if child exists
    }                                                           //  End loop over child cells
  }                                                             // End while loop for non empty queue
#ifdef TASK_GROUP
  tbb::task_group tg;                                           // Create task group
  for (int c=0; c<cellVector.size(); c++) {                     // Loop over cells
    Cell * C = cellVector[c];                                   //  Current cell
    Evaluate evaluate(C);                                       //  Instantiate functor
    tg.run(evaluate);                                           //  If task is large, spawn new task
  }                                                             // End loop over cells
  tg.wait();                                                    // Sync threads
#elif OPENMP
#pragma omp parallel for schedule(static,100) num_threads(numWorkers) 
  for (int c=0; c<cellVector.size(); c++) {                     // Loop over cells
    Cell * C = cellVector[c];                                   //  Current cell
    Evaluate evaluate(C);                                       //  Instantiate functor
    evaluate();                                                 //  If task is large, spawn new task
  }                                                             // End loop over cells
#elif TAO

  for (int i=0; i < NA; i++)
     gotao_push_init(fmms[i]);

  gotao_start();
  gotao_fini();

#else // PARALLEL_FOR
#error "PARALLEL_FOR is currently not working. Use OpenMP or TBB Task Groups for the while :-)"
//  tbb::parallel_for(0, cellVector.size(), [&](int i) {
//    Cell * C = cellVector[i];                                   //  Current cell
//    Evaluate evaluate(C);                                       //  Instantiate functor
//    evaluate();                                                 //  run functor
//    })
#endif
}
#endif

//! Recursive call for downward pass
void downwardPass(Cell * C) {
#ifdef LIST
  //for (int i=0; i<C->M2L.size(); i++) M2L(C,C->M2L[i]);         // M2L kernel
#endif
  L2L(C);                                                       // L2L kernel
  if (C->NNODE == 1) {
#ifdef LIST
    //for (int i=0; i<C->P2P.size(); i++) P2P(C,C->P2P[i]);       // P2P kernel
#endif
    L2P(C);                                                     // L2P kernel
  }
  for (int i=0; i<4; i++) {                                     // Loop over child cells
    if (C->CHILD[i]) downwardPass(C->CHILD[i]);                 //  Recursive call with new task
  }                                                             // End loop over child cells
}
  
//! Direct summation
void direct(int ni, Body * ibodies, int nj, Body * jbodies) {
  Cell * Ci = new Cell();                                       // Allocate single target cell
  Cell * Cj = new Cell();                                       // Allocate single source cell
  Ci->BODY = ibodies;                                           // Pointer of first target body
  Ci->NBODY = ni;                                               // Number of target bodies
  Cj->BODY = jbodies;                                           // Pointer of first source body
  Cj->NBODY = nj;                                               // Number of source bodies
  P2P(Ci, Cj);                                                  // Evaluate P2P kernel
}
#endif
