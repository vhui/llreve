#include "DRM.h"

#include "core/Util.h"

using namespace std;
using namespace llvm;

LinearizedFunction::LinearizedFunction(
		Function& func) {
	
	int index = 0;
	
	for(Instruction& i : Util::getInstructions(func)) {
		mapInstToInt[&i] = index++;
	}
	
	// 'index' is the number of instructions
	mapIntToInst = new Instruction*[index];
	
	for(Instruction& i : Util::getInstructions(func)) {
		// use the previous generated mapping as the iteration may vary
		// between different iterations
		mapIntToInst[mapInstToInt[&i]] = &i;
	}
}
	
LinearizedFunction::~LinearizedFunction(void) {
	
	delete [] mapIntToInst;
}

Instruction& operator[](
		unsigned int const index) const {
	
	return *mapIntToInst[index];
}

unsigned int operator[](
		Instruction const& inst)  const {
	
	return mapInstToInt[&inst];
}

BitArray const& DRM::computeSlice(
		BitArray const& apriori) {
	
	accumulator.reset();
	
	for(unsigned int i = 0; i < apriori.size; i++) {
		if(apriori[i]) {
			accumulator |= matrix[i];
		}
	}
	
	return accumulator;
}