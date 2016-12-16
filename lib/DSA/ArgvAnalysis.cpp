//===-- ArgvAnalysis.cpp - Finding Argv -----------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"

#include "dsa/ArgvAnalysis.h"

using namespace llvm;

void ArgvAnalysis::handleUsers(Value *V) {
  for (User *U : V->users()) {
    if (isa<CastInst>(U)) {
      // TODO: handle CastInst
      continue;
    }

    if (CallInst *CI = dyn_cast<CallInst>(U)) {
      Function *F = CI->getCalledFunction();
      if (F->isDeclaration())
        continue;

      int ArgvArgNum = -1;
      for (int i = 0, e = CI->getNumArgOperands(); i < e; i++) {
        if (CI->getArgOperand(i) == V) {
          assert(ArgvArgNum == -1 && "argv passed in multiple args?");
          ArgvArgNum = i;
        }
      }

      assert(ArgvArgNum != -1 && "argv not passed as an argument!");

      handleFunction(CI->getCalledFunction(), ArgvArgNum);
      continue;
    }

    if (isa<GetElementPtrInst>(U)) {
      // TODO: handle CastInst
      continue;
    }

    if (isa<LoadInst>(U))
      continue;

    if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
      Value *Dst = SI->getPointerOperand();
      assert(isa<GlobalVariable>(Dst) && "currently only handle store to GV");
      ArgvGVs.insert(cast<GlobalVariable>(Dst));
      continue;
    }

    assert(0);
  }
}

void ArgvAnalysis::handleFunction(Function *F, int ArgNum) {
  Function::arg_iterator AI = F->arg_begin();
  std::advance(AI, ArgNum);
  Value *Argv = &*AI;
  ArgvValues.insert(Argv);

  handleUsers(Argv);
}

bool ArgvAnalysis::runOnModule(llvm::Module& M) {
  Function *Main = M.getFunction("main");
  handleFunction(Main, 1);

  for (Value *V : ArgvGVs) {
    for (User *U : V->users()) {
      if (isa<StoreInst>(U))
        continue;

      assert(isa<LoadInst>(U));
      ArgvValues.insert(U);
      handleUsers(U);
    }
  }

  return false;
}

void ArgvAnalysis::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
  AU.setPreservesAll();
}

char ArgvAnalysis::ID;

static RegisterPass<ArgvAnalysis> A("argv-analysis", "Identify IR Values related to Argv");
