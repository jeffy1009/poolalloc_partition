//===-- PoolAllocate.cpp - Pool Allocation Pass ---------------------------===//
//
// This transform changes programs so that disjoint data structures are
// allocated out of different pools of memory, increasing locality.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "PoolAllocation"
#include "poolalloc/PoolAllocate.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Analysis/DataStructure.h"
#include "llvm/Analysis/DSGraph.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Target/TargetData.h"
#include "Support/Debug.h"
#include "Support/VectorExtras.h"
#include "Support/Statistic.h"
using namespace PA;

const Type *PoolAllocate::PoolDescPtrTy = 0;

namespace {
  Statistic<> NumArgsAdded("poolalloc", "Number of function arguments added");
  Statistic<> NumCloned   ("poolalloc", "Number of functions cloned");
  Statistic<> NumPools    ("poolalloc", "Number of poolinit's inserted");

  const Type *VoidPtrTy;

  // The type to allocate for a pool descriptor: { sbyte*, uint, uint }
  // void *Data (the data)
  // unsigned NodeSize  (size of an allocated node)
  // unsigned FreeablePool (are slabs in the pool freeable upon calls to 
  //                        poolfree?)
  const Type *PoolDescType;
  
  RegisterOpt<PoolAllocate>
  X("poolalloc", "Pool allocate disjoint data structures");
}

void PoolAllocate::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<BUDataStructures>();
  AU.addRequired<TDDataStructures>();
  AU.addRequired<TargetData>();
}

// Prints out the functions mapped to the leader of the equivalence class they
// belong to.
void PoolAllocate::printFuncECs() {
  std::map<Function*, Function*> &leaderMap = FuncECs.getLeaderMap();
  std::cerr << "Indirect Function Map \n";
  for (std::map<Function*, Function*>::iterator LI = leaderMap.begin(),
	 LE = leaderMap.end(); LI != LE; ++LI) {
    std::cerr << LI->first->getName() << ": leader is "
	      << LI->second->getName() << "\n";
  }
}

static void printNTOMap(std::map<Value*, const Value*> &NTOM) {
  std::cerr << "NTOM MAP\n";
  for (std::map<Value*, const Value *>::iterator I = NTOM.begin(), 
	 E = NTOM.end(); I != E; ++I) {
    if (!isa<Function>(I->first) && !isa<BasicBlock>(I->first))
      std::cerr << *I->first << " to " << *I->second << "\n";
  }
}

void PoolAllocate::buildIndirectFunctionSets(Module &M) {
  // Iterate over the module looking for indirect calls to functions

  // Get top down DSGraph for the functions
  TDDS = &getAnalysis<TDDataStructures>();
  
  for (Module::iterator MI = M.begin(), ME = M.end(); MI != ME; ++MI) {

    DEBUG(std::cerr << "Processing indirect calls function:" <<  MI->getName()
                    << "\n");

    if (MI->isExternal())
      continue;

    DSGraph &TDG = TDDS->getDSGraph(*MI);

    const std::vector<DSCallSite> &callSites = TDG.getFunctionCalls();

    // For each call site in the function
    // All the functions that can be called at the call site are put in the
    // same equivalence class.
    for (std::vector<DSCallSite>::const_iterator CSI = callSites.begin(), 
	   CSE = callSites.end(); CSI != CSE ; ++CSI) {
      if (CSI->isIndirectCall()) {
	DSNode *DSN = CSI->getCalleeNode();
	if (DSN->isIncomplete())
	  std::cerr << "Incomplete node: "
                    << *CSI->getCallSite().getInstruction();
	// assert(DSN->isGlobalNode());
	const std::vector<GlobalValue*> &Callees = DSN->getGlobals();
	if (Callees.empty())
	  std::cerr << "No targets: " << *CSI->getCallSite().getInstruction();
        Function *RunningClass = 0;
        for (std::vector<GlobalValue*>::const_iterator CalleesI = 
               Callees.begin(), CalleesE = Callees.end(); 
             CalleesI != CalleesE; ++CalleesI)
          if (Function *calledF = dyn_cast<Function>(*CalleesI)) {
            CallSiteTargets.insert(std::make_pair(CSI->getCallSite(), calledF));
            if (RunningClass == 0) {
              RunningClass = calledF;
              FuncECs.addElement(RunningClass);
            } else {
              FuncECs.unionSetsWith(RunningClass, calledF);
            }
          }
      }
    }
  }
  
  // Print the equivalence classes
  DEBUG(printFuncECs());
}

bool PoolAllocate::run(Module &M) {
  if (M.begin() == M.end()) return false;
  CurModule = &M;
  BU = &getAnalysis<BUDataStructures>();

  if (VoidPtrTy == 0) {
    VoidPtrTy = PointerType::get(Type::SByteTy);
    PoolDescType =
      StructType::get(make_vector<const Type*>(VoidPtrTy, VoidPtrTy,
                                               Type::UIntTy, Type::UIntTy, 0));
    PoolDescPtrTy = PointerType::get(PoolDescType);
  }
  
  AddPoolPrototypes();
  buildIndirectFunctionSets(M);

  std::map<Function*, Function*> FuncMap;

  // Loop over the functions in the original program finding the pool desc.
  // arguments necessary for each function that is indirectly callable.
  // For each equivalence class, make a list of pool arguments and update
  // the PoolArgFirst and PoolArgLast values for each function.
  Module::iterator LastOrigFunction = --M.end();
  for (Module::iterator I = M.begin(); ; ++I) {
    if (!I->isExternal())
      FindFunctionPoolArgs(*I);
    if (I == LastOrigFunction) break;
  }

  // Now clone a function using the pool arg list obtained in the previous
  // pass over the modules.
  // Loop over only the function initially in the program, don't traverse newly
  // added ones.  If the function uses memory, make its clone.
  for (Module::iterator I = M.begin(); ; ++I) {
    if (!I->isExternal())
      if (Function *R = MakeFunctionClone(*I))
        FuncMap[I] = R;
    if (I == LastOrigFunction) break;
  }
  
  ++LastOrigFunction;

  // Now that all call targets are available, rewrite the function bodies of the
  // clones.
  for (Module::iterator I = M.begin(); I != LastOrigFunction; ++I)
    if (!I->isExternal()) {
      std::map<Function*, Function*>::iterator FI = FuncMap.find(I);
      ProcessFunctionBody(*I, FI != FuncMap.end() ? *FI->second : *I);
    }

  if (CollapseFlag)
    std::cerr << "Pool Allocation successful!"
              << " However all data structures may not be pool allocated\n";
  return true;
}


// AddPoolPrototypes - Add prototypes for the pool functions to the specified
// module and update the Pool* instance variables to point to them.
//
void PoolAllocate::AddPoolPrototypes() {
  CurModule->addTypeName("PoolDescriptor", PoolDescType);
  
  // Get poolinit function...
  PoolInit = CurModule->getOrInsertFunction("poolinit", Type::VoidTy,
                                            PoolDescPtrTy, Type::UIntTy, 0);

  // Get pooldestroy function...
  PoolDestroy = CurModule->getOrInsertFunction("pooldestroy", Type::VoidTy,
                                               PoolDescPtrTy, 0);
  
  // The poolalloc function
  PoolAlloc = CurModule->getOrInsertFunction("poolalloc", 
                                             VoidPtrTy, PoolDescPtrTy,
                                             Type::UIntTy, 0);
  
  // Get the poolfree function...
  PoolFree = CurModule->getOrInsertFunction("poolfree", Type::VoidTy,
                                            PoolDescPtrTy, VoidPtrTy, 0);  
}

// Inline the DSGraphs of functions corresponding to the potential targets at
// indirect call sites into the DS Graph of the callee.
// This is required to know what pools to create/pass at the call site in the 
// caller
//
void PoolAllocate::InlineIndirectCalls(Function &F, DSGraph &G, 
                                       hash_set<Function*> &visited) {
  const std::vector<DSCallSite> &callSites = G.getFunctionCalls();
  
  visited.insert(&F);

  // For each indirect call site in the function, inline all the potential
  // targets
  for (std::vector<DSCallSite>::const_iterator CSI = callSites.begin(),
         CSE = callSites.end(); CSI != CSE; ++CSI) {
    if (CSI->isIndirectCall()) {
      CallSite CS = CSI->getCallSite();
      std::pair<std::multimap<CallSite, Function*>::iterator,
        std::multimap<CallSite, Function*>::iterator> Targets =
        CallSiteTargets.equal_range(CS);
      for (std::multimap<CallSite, Function*>::iterator TFI = Targets.first,
             TFE = Targets.second; TFI != TFE; ++TFI)
        if (!TFI->second->isExternal()) {
          DSGraph &TargetG = BU->getDSGraph(*TFI->second);
          // Call the function recursively if the callee is not yet inlined and
          // if it hasn't been visited in this sequence of calls The latter is
          // dependent on the fact that the graphs of all functions in an SCC
          // are actually the same
          if (InlinedFuncs.find(TFI->second) == InlinedFuncs.end() && 
              visited.find(TFI->second) == visited.end()) {
            InlineIndirectCalls(*TFI->second, TargetG, visited);
          }
          G.mergeInGraph(*CSI, *TFI->second, TargetG, DSGraph::KeepModRefBits | 
                         DSGraph::KeepAllocaBit | DSGraph::DontCloneCallNodes |
                         DSGraph::DontCloneAuxCallNodes); 
        }
    }
  }
  
  // Mark this function as one whose graph is inlined with its indirect 
  // function targets' DS Graphs.  This ensures that every function is inlined
  // exactly once
  InlinedFuncs.insert(&F);
}

void PoolAllocate::FindFunctionPoolArgs(Function &F) {

  DSGraph &G = BU->getDSGraph(F);
 
  // Inline the potential targets of indirect calls
  hash_set<Function*> visitedFuncs;
  InlineIndirectCalls(F, G, visitedFuncs);

  // The DSGraph is merged with the globals graph. 
  G.mergeInGlobalsGraph();

  // The nodes reachable from globals need to be recognized as potential 
  // arguments. This is required because, upon merging in the globals graph,
  // the nodes pointed to by globals that are not live are not marked 
  // incomplete.
  hash_set<DSNode*> NodesFromGlobals;
  for (DSGraph::ScalarMapTy::iterator I = G.getScalarMap().begin(), 
	 E = G.getScalarMap().end(); I != E; ++I)
    if (isa<GlobalValue>(I->first)) {             // Found a global
      DSNodeHandle &GH = I->second;
      GH.getNode()->markReachableNodes(NodesFromGlobals);
    }

  // At this point the DS Graphs have been modified in place including
  // information about globals as well as indirect calls, making it useful
  // for pool allocation
  std::vector<DSNode*> &Nodes = G.getNodes();
  if (Nodes.empty()) return ;  // No memory activity, nothing is required

  FuncInfo &FI = FunctionInfo[&F];   // Create a new entry for F
  
  FI.Clone = 0;
  
  // Initialize the PoolArgFirst and PoolArgLast for the function depending
  // on whether there have been other functions in the equivalence class
  // that have pool arguments so far in the analysis.
  if (!FuncECs.findClass(&F)) {
    FI.PoolArgFirst = FI.PoolArgLast = 0;
  } else {
    if (EqClass2LastPoolArg.find(FuncECs.findClass(&F)) != 
	EqClass2LastPoolArg.end())
      FI.PoolArgFirst = FI.PoolArgLast = 
	EqClass2LastPoolArg[FuncECs.findClass(&F)] + 1;
    else
      FI.PoolArgFirst = FI.PoolArgLast = 0;
  }
  
  // Find DataStructure nodes which are allocated in pools non-local to the
  // current function.  This set will contain all of the DSNodes which require
  // pools to be passed in from outside of the function.
  hash_set<DSNode*> &MarkedNodes = FI.MarkedNodes;
  
  // Mark globals and incomplete nodes as live... (this handles arguments)
  if (F.getName() != "main")
    for (unsigned i = 0, e = Nodes.size(); i != e; ++i) {
      if (Nodes[i]->isGlobalNode() && !Nodes[i]->isIncomplete())
        DEBUG(std::cerr << "Global node is not Incomplete\n");
      if ((Nodes[i]->isIncomplete() || Nodes[i]->isGlobalNode() || 
	   NodesFromGlobals.count(Nodes[i])) && Nodes[i]->isHeapNode())
        Nodes[i]->markReachableNodes(MarkedNodes);
    }

  // Marked the returned node as alive...
  if (DSNode *RetNode = G.getReturnNodeFor(F).getNode())
    if (RetNode->isHeapNode())
      RetNode->markReachableNodes(MarkedNodes);

  if (MarkedNodes.empty())   // We don't need to clone the function if there
    return;                  // are no incoming arguments to be added.

  // Erase any marked node that is not a heap node

  for (hash_set<DSNode*>::iterator I = MarkedNodes.begin(),
	 E = MarkedNodes.end(); I != E; ) {
    // erase invalidates hash_set iterators if the iterator points to the
    // element being erased
    if (!(*I)->isHeapNode())
      MarkedNodes.erase(I++);
    else
      ++I;
  }

  FI.PoolArgLast += MarkedNodes.size();

  // Update the equivalence class last pool argument information
  // only if there actually were pool arguments to the function.
  // Also, there is no entry for the Eq. class in EqClass2LastPoolArg
  // if there are no functions in the equivalence class with pool arguments.
  if (FuncECs.findClass(&F) && FI.PoolArgLast != FI.PoolArgFirst)
    EqClass2LastPoolArg[FuncECs.findClass(&F)] = FI.PoolArgLast - 1;
}

// MakeFunctionClone - If the specified function needs to be modified for pool
// allocation support, make a clone of it, adding additional arguments as
// necessary, and return it.  If not, just return null.
//
Function *PoolAllocate::MakeFunctionClone(Function &F) {
  DSGraph &G = BU->getDSGraph(F);
  std::vector<DSNode*> &Nodes = G.getNodes();
  if (Nodes.empty()) return 0;
    
  FuncInfo &FI = FunctionInfo[&F];
  
  hash_set<DSNode*> &MarkedNodes = FI.MarkedNodes;
  
  if (!FuncECs.findClass(&F)) {
    // Not in any equivalence class
    if (MarkedNodes.empty())
      return 0;
  } else {
    // No need to clone if there are no pool arguments in any function in the
    // equivalence class
    if (!EqClass2LastPoolArg.count(FuncECs.findClass(&F)))
      return 0;
  }
      
  // Figure out what the arguments are to be for the new version of the function
  const FunctionType *OldFuncTy = F.getFunctionType();
  std::vector<const Type*> ArgTys;
  if (!FuncECs.findClass(&F)) {
    ArgTys.reserve(OldFuncTy->getParamTypes().size() + MarkedNodes.size());
    FI.ArgNodes.reserve(MarkedNodes.size());
    for (hash_set<DSNode*>::iterator I = MarkedNodes.begin(),
	   E = MarkedNodes.end(); I != E; ++I) {
      ArgTys.push_back(PoolDescPtrTy);    // Add the appropriate # of pool descs
      FI.ArgNodes.push_back(*I);
    }
    if (FI.ArgNodes.empty()) return 0;      // No nodes to be pool allocated!

  }
  else {
    // This function is a member of an equivalence class and needs to be cloned 
    ArgTys.reserve(OldFuncTy->getParamTypes().size() + 
		   EqClass2LastPoolArg[FuncECs.findClass(&F)] + 1);
    FI.ArgNodes.reserve(EqClass2LastPoolArg[FuncECs.findClass(&F)] + 1);
    
    for (int i = 0; i <= EqClass2LastPoolArg[FuncECs.findClass(&F)]; ++i)
      ArgTys.push_back(PoolDescPtrTy);    // Add the appropriate # of pool descs

    for (hash_set<DSNode*>::iterator I = MarkedNodes.begin(),
	   E = MarkedNodes.end(); I != E; ++I) {
      FI.ArgNodes.push_back(*I);
    }

    assert((FI.ArgNodes.size() == (unsigned)(FI.PoolArgLast-FI.PoolArgFirst)) &&
           "Number of ArgNodes equal to the number of pool arguments used by "
           "this function");

    if (FI.ArgNodes.empty()) return 0;
  }
      
  NumArgsAdded += ArgTys.size();
  ++NumCloned;

  ArgTys.insert(ArgTys.end(), OldFuncTy->getParamTypes().begin(),
                OldFuncTy->getParamTypes().end());


  // Create the new function prototype
  FunctionType *FuncTy = FunctionType::get(OldFuncTy->getReturnType(), ArgTys,
                                           OldFuncTy->isVarArg());
  // Create the new function...
  Function *New = new Function(FuncTy, GlobalValue::InternalLinkage,
                               F.getName(), F.getParent());

  // Set the rest of the new arguments names to be PDa<n> and add entries to the
  // pool descriptors map
  std::map<DSNode*, Value*> &PoolDescriptors = FI.PoolDescriptors;
  //Dinakar set the type of pooldesctriptors
  std::map<const Value*, const Type*> &PoolDescTypeMap = FI.PoolDescType;
  Function::aiterator NI = New->abegin();
  
  if (FuncECs.findClass(&F)) {
    // If the function belongs to an equivalence class
    for (int i = 0; i <= EqClass2LastPoolArg[FuncECs.findClass(&F)]; ++i, 
	   ++NI)
      NI->setName("PDa");
    
    NI = New->abegin();
    if (FI.PoolArgFirst > 0)
      for (int i = 0; i < FI.PoolArgFirst; ++NI, ++i)
	;

    for (unsigned i = 0, e = FI.ArgNodes.size(); i != e; ++i, ++NI) {
      PoolDescTypeMap[NI] = FI.ArgNodes[i]->getType();
      
      PoolDescriptors.insert(std::make_pair(FI.ArgNodes[i], NI));
    }
    NI = New->abegin();
    if (EqClass2LastPoolArg.count(FuncECs.findClass(&F)))
      for (int i = 0; i <= EqClass2LastPoolArg[FuncECs.findClass(&F)]; ++i,++NI)
	;
  } else {
    // If the function does not belong to an equivalence class
    if (FI.ArgNodes.size())
      for (unsigned i = 0, e = FI.ArgNodes.size(); i != e; ++i, ++NI) {
	NI->setName("PDa");  // Add pd entry
	PoolDescTypeMap[NI] = FI.ArgNodes[i]->getType();
	PoolDescriptors.insert(std::make_pair(FI.ArgNodes[i], NI));
      }
    NI = New->abegin();
    if (FI.ArgNodes.size())
      for (unsigned i = 0; i < FI.ArgNodes.size(); ++NI, ++i)
	;
  }

  // Map the existing arguments of the old function to the corresponding
  // arguments of the new function.
  std::map<const Value*, Value*> ValueMap;
  if (NI != New->aend()) 
    for (Function::aiterator I = F.abegin(), E = F.aend(); I != E; ++I, ++NI) {
      ValueMap[I] = NI;
      NI->setName(I->getName());
    }

  // Populate the value map with all of the globals in the program.
  // FIXME: This should be unnecessary!
  Module &M = *F.getParent();
  for (Module::iterator I = M.begin(), E=M.end(); I!=E; ++I)    ValueMap[I] = I;
  for (Module::giterator I = M.gbegin(), E=M.gend(); I!=E; ++I) ValueMap[I] = I;

  // Perform the cloning.
  std::vector<ReturnInst*> Returns;
  CloneFunctionInto(New, &F, ValueMap, Returns);

  // Invert the ValueMap into the NewToOldValueMap
  std::map<Value*, const Value*> &NewToOldValueMap = FI.NewToOldValueMap;
  for (std::map<const Value*, Value*>::iterator I = ValueMap.begin(),
         E = ValueMap.end(); I != E; ++I)
    NewToOldValueMap.insert(std::make_pair(I->second, I->first));
  
  return FI.Clone = New;
}


// CreatePools - This creates the pool initialization and destruction code for
// the DSNodes specified by the NodesToPA list.  This adds an entry to the
// PoolDescriptors map for each DSNode.
//
void PoolAllocate::CreatePools(Function &F,
                               const std::vector<DSNode*> &NodesToPA,
                               std::map<DSNode*, Value*> &PoolDescriptors,
			       std::map<const Value *,
                                        const Type *> &PoolDescTypeMap) {
  // Find all of the return nodes in the CFG...
  std::vector<BasicBlock*> ReturnNodes;
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
    if (isa<ReturnInst>(I->getTerminator()))
      ReturnNodes.push_back(I);

  TargetData &TD = getAnalysis<TargetData>();

  // Loop over all of the pools, inserting code into the entry block of the
  // function for the initialization and code in the exit blocks for
  // destruction.
  //
  Instruction *InsertPoint = F.front().begin();
  for (unsigned i = 0, e = NodesToPA.size(); i != e; ++i) {
    DSNode *Node = NodesToPA[i];
    
    // Create a new alloca instruction for the pool...
    Value *AI = new AllocaInst(PoolDescType, 0, "PD", InsertPoint);
    const Type *Eltype;
    Value *ElSize;
    
    // Void types in DS graph are never used
    if (Node->getType() != Type::VoidTy) {
      ElSize = ConstantUInt::get(Type::UIntTy, TD.getTypeSize(Node->getType()));
      Eltype = Node->getType();
    } else {
      std::cerr << "Node collapsing in '" << F.getName() 
		<< "'. All Data Structures may not be pool allocated\n";
      ElSize = ConstantUInt::get(Type::UIntTy, 1);
    }
    
    // Insert the call to initialize the pool...
    new CallInst(PoolInit, make_vector(AI, ElSize, 0), "", InsertPoint);
    ++NumPools;
      
    // Update the PoolDescriptors map
    PoolDescriptors.insert(std::make_pair(Node, AI));
    PoolDescTypeMap[AI] = Eltype;
    
    // Insert a call to pool destroy before each return inst in the function
    for (unsigned r = 0, e = ReturnNodes.size(); r != e; ++r)
      new CallInst(PoolDestroy, make_vector(AI, 0), "",
		   ReturnNodes[r]->getTerminator());
  }
}



// processFunction - Pool allocate any data structures which are contained in
// the specified function...
//
void PoolAllocate::ProcessFunctionBody(Function &F, Function &NewF) {
  DSGraph &G = BU->getDSGraph(F);

  std::vector<DSNode*> &Nodes = G.getNodes();
  if (Nodes.empty()) return;     // Quick exit if nothing to do...
  
  FuncInfo &FI = FunctionInfo[&F];   // Get FuncInfo for F
  hash_set<DSNode*> &MarkedNodes = FI.MarkedNodes;
  
  DEBUG(std::cerr << "[" << F.getName() << "] Pool Allocate: ");
  
  // Loop over all of the nodes which are non-escaping, adding pool-allocatable
  // ones to the NodesToPA vector.
  std::vector<DSNode*> NodesToPA;
  for (unsigned i = 0, e = Nodes.size(); i != e; ++i)
    if (Nodes[i]->isHeapNode() &&   // Pick nodes with heap elems
        !MarkedNodes.count(Nodes[i]))              // Can't be marked
      NodesToPA.push_back(Nodes[i]);
  
  DEBUG(std::cerr << NodesToPA.size() << " nodes to pool allocate\n");
  if (!NodesToPA.empty()) {
    // Create pool construction/destruction code
    std::map<DSNode*, Value*> &PoolDescriptors = FI.PoolDescriptors;
    std::map<const Value*, const Type*> &PoolDescTypeMap = FI.PoolDescType;
    CreatePools(NewF, NodesToPA, PoolDescriptors, PoolDescTypeMap);
  }
  
  // Transform the body of the function now... collecting information about uses
  // of the pools.
  std::set<std::pair<AllocaInst*, BasicBlock*> > PoolUses;
  std::set<std::pair<AllocaInst*, CallInst*> > PoolFrees;
  TransformBody(G, TDDS->getDSGraph(F), FI, PoolUses, PoolFrees, NewF);
}

