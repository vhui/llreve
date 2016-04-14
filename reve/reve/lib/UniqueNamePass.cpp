#include "UniqueNamePass.h"

#include <iostream>

using std::string;

using llvm::PreservedAnalyses;

PreservedAnalyses UniqueNamePass::run(llvm::Function &F,
                                      llvm::FunctionAnalysisManager *AM) {
    (void)AM;
    std::map<string, int> InstructionNames;
    std::string funName = F.getName();

    for (auto &Arg : F.args()) {
        makePrefixed(Arg, Prefix, InstructionNames);
    }

    int i = 0;
    for (auto &Block : F) {
        Block.setName(funName + "/" + std::to_string(i));
        for (auto &Inst : Block) {
            makePrefixed(Inst, Prefix, InstructionNames);
        }
    }
    return PreservedAnalyses::all();
}

void makePrefixed(llvm::Value &Val, std::string Prefix,
                  std::map<string, int> &InstructionNames) {
    // Note: The identity of values is not given by their names, so a
    // lot of register are simply unnamed. The numbers you see in the
    // output are only created when converting the assembly to a
    // string, so for most values, we simply set the complete name
    // here.

    // It is illegal to specify names for void instructions
    if (!Val.getType()->isVoidTy()) {
        string OldName = Val.getName();
        if (OldName == "") {
            OldName = "_"; // Make valid name for smt solvers
        }
        if (InstructionNames.find(OldName) == InstructionNames.end()) {
            InstructionNames.insert(std::make_pair(OldName, 0));
        }
        Val.setName(OldName + "$" + Prefix + "_" +
                    std::to_string(InstructionNames.at(OldName)++));
    }
}
