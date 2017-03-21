/*
 * This file is part of
 *    llreve - Automatic regression verification for LLVM programs
 *
 * Copyright (C) 2016 Karlsruhe Institute of Technology
 *
 * The system is published under a BSD license.
 * See LICENSE (distributed with this file) for details.
 */

#include "AnnotStackPass.h"

#include "Helper.h"
#include "MarkAnalysis.h"
#include "PathAnalysis.h"

#include <iostream>

char AnnotStackPass::ID = 0;

bool AnnotStackPass::runOnFunction(llvm::Function &F) {
    int StackIndex = -1;
    std::set<llvm::Value *> StackOps;
    for (auto &Block : F) {
        for (auto &Inst : Block) {
            if (auto AllocaInst = llvm::dyn_cast<llvm::AllocaInst>(&Inst)) {
                StackOps.insert(AllocaInst);
                StackIndex -=
                    typeSize(AllocaInst->getAllocatedType(),
                             AllocaInst->getModule()->getDataLayout());
                markStackInstruction(*AllocaInst, "reve.stack_pointer",
                                     StackIndex);
            } else if (auto GetElementPtr =
                           llvm::dyn_cast<llvm::GetElementPtrInst>(&Inst)) {
                if (StackOps.find(GetElementPtr->getPointerOperand()) !=
                    StackOps.end()) {
                    StackOps.insert(GetElementPtr);
                }
            } else if (auto CastInst = llvm::dyn_cast<llvm::CastInst>(&Inst)) {
                if (StackOps.find(CastInst->getOperand(0)) != StackOps.end()) {
                    StackOps.insert(CastInst);
                }
            } else if (auto LoadInst = llvm::dyn_cast<llvm::LoadInst>(&Inst)) {
                if (StackOps.find(LoadInst->getPointerOperand()) !=
                    StackOps.end()) {
                    StackOps.insert(LoadInst);
                }
            } else if (auto StoreInst =
                           llvm::dyn_cast<llvm::StoreInst>(&Inst)) {
                if (StackOps.find(StoreInst->getPointerOperand()) !=
                    StackOps.end()) {
                    StackOps.insert(StoreInst);
                }
            }
        }
    }
    for (auto StackOp : StackOps) {
        if (auto Inst = llvm::dyn_cast<llvm::Instruction>(StackOp)) {
            markStackInstruction(*Inst, "reve.stackop", 0);
        }
    }
    return true;
}

void AnnotStackPass::getAnalysisUsage(llvm::AnalysisUsage &AU) const {
    AU.addPreserved<MarkAnalysis>();
    AU.addPreserved<PathAnalysis>();
    AU.setPreservesCFG();
}

void markStackInstruction(llvm::Instruction &Inst, std::string MetadataName,
                          int Pointer) {
    llvm::LLVMContext &Ctxt = Inst.getContext();
    llvm::MDNode *N = llvm::MDNode::get(
        Ctxt,
        llvm::MDString::get(Ctxt, "(- " + std::to_string(-Pointer) + ")"));
    Inst.setMetadata(MetadataName, N);
}

static llvm::RegisterPass<AnnotStackPass>
    RemoveMarkPass("annotate-stack", "Annotate stack instructions", false,
                   false);