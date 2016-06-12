#ifndef PROMOTEASSERTSLICEDPASS_H
#define PROMOTEASSERTSLICEDPASS_H

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Constants.h"

class PromoteAssertSlicedPass: public llvm::FunctionPass {

public:
	static char ID;
	static std::string ASSERT_SLICED;
    static std::string FUNCTION_NAME;
	static void markAssertSliced(llvm::Instruction& instruction);
	static bool isAssertSliced(llvm::Instruction& instruction);

	PromoteAssertSlicedPass();

	virtual ~PromoteAssertSlicedPass();
	virtual bool runOnFunction(llvm::Function &Fun) override;
	virtual void getAnalysisUsage(llvm::AnalysisUsage &AU) const override;



protected:

private:
};

#endif // PROMOTEASSERTSLICEDPASS_H
