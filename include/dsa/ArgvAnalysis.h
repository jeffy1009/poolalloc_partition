//===-- ArgvAnalysis.h - --------------------------------------------------===//
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

#ifndef _ARGVANALYSIS_H
#define	_ARGVANALYSIS_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Pass.h"

namespace llvm {

class Value;

class ArgvAnalysis : public ModulePass {
  SmallPtrSet<Value*, 4> ArgvValues;
  SmallPtrSet<GlobalVariable*, 4> ArgvGVs;

public:
  static char ID;
  ArgvAnalysis() : ModulePass(ID) {}

  bool runOnModule(Module&);

  virtual void getAnalysisUsage(AnalysisUsage &Info) const;

  const SmallPtrSetImpl<Value*> &getArgvValues() const {
    return ArgvValues;
  }

private:
  void handleUsers(Value *V);
  void handleFunction(Function *F, int ArgNum);
};

}

#endif
