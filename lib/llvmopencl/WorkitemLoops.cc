// LLVM function pass to create a loop that runs all the work items 
// in a work group.
// 
// Copyright (c) 2012 Pekka Jääskeläinen / TUT
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#define DEBUG_TYPE "workitem-loops"

#include "WorkitemLoops.h"
#include "Workgroup.h"
#include "Barrier.h"
#include "Kernel.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/IRBuilder.h"
#include "llvm/Support/TypeBuilder.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ValueSymbolTable.h"
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

#ifdef DUMP_RESULT_CFG
#include "llvm/Analysis/CFGPrinter.h"
#endif

//#define DEBUG_WORK_ITEM_LOOPS

using namespace llvm;
using namespace pocl;

namespace {
  static
  RegisterPass<WorkitemLoops> X("workitemloops", "Workitem loop generation pass");
}

char WorkitemLoops::ID = 0;

void
WorkitemLoops::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.addRequired<DominatorTree>();
  AU.addRequired<LoopInfo>();
  AU.addRequired<TargetData>();
}

bool
WorkitemLoops::runOnFunction(Function &F)
{
  if (!Workgroup::isKernelToProcess(F))
    return false;

  DT = &getAnalysis<DominatorTree>();
  LI = &getAnalysis<LoopInfo>();

  llvm::Module *M = F.getParent();
  llvm::Type *localIdType; 
  if (M->getPointerSize() == llvm::Module::Pointer64)
    size_t_width = 64;
  else if (M->getPointerSize() == llvm::Module::Pointer32)
    size_t_width = 32;
  else
    assert (false && "Only 32 and 64 bit size_t widths supported.");

  localIdType = IntegerType::get(F.getContext(), size_t_width);

  localIdZ = M->getOrInsertGlobal("_local_id_z", localIdType);
  localIdY = M->getOrInsertGlobal("_local_id_y", localIdType);
  localIdX = M->getOrInsertGlobal("_local_id_x", localIdType);

  bool changed = ProcessFunction(F);
#ifdef DUMP_RESULT_CFG
  FunctionPass* cfgPrinter = createCFGPrinterPass();
  cfgPrinter->runOnFunction(F);
#endif

  //  changed |= fixUndominatedVariableUses(DT, F);
  return changed;
}

void
WorkitemLoops::CreateLoopAround
(llvm::BasicBlock *entryBB, llvm::BasicBlock *exitBB, llvm::Value *localIdVar, 
 size_t LocalSizeForDim) 
{
#ifdef DEBUG_WORK_ITEM_LOOPS
  std::cerr << "### creating a loop around PR:" << std::endl;
  region->dumpNames();    
#endif
  assert (localIdVar != NULL);

  /* TODO: Process the variables: in case the variable is 
     1) created before the PR, load its value from the WI context
     data array
     2) live only inside the PR: do nothing
     3) created inside the PR and used outside, save its value
     at the end of the loop to the WI context data array */

  /* TODO: The Conditional barrier case needs the first iteration
     peeled. Check the case how it's best done. */

  /*

    Generate a structure like this for each loop level (x,y,z):

    for.inc:
    %2 = load i32* %_local_id_x, align 4
    %inc = add nsw i32 %2, 1
    store i32 %inc, i32* %_local_id_x, align 4
    br label %for.cond

    for.cond:
    ; loop header, compare the id to the local size
    %0 = load i32* %_local_id_x, align 4
    %cmp = icmp ult i32 %0, i32 123
    br i1 %cmp, label %for.body, label %for.end

    for.body: 

    ; the parallel region code here

    br label %for.inc
        

    for.end:

    OPTIMIZE: Use a separate iteration variable across all the loops to iterate the context 
    data arrays to avoid needing multiplications to find the correct location, and to 
    enable easy vectorization of loading the context data when there are parallel iterations.
  */     


  llvm::BasicBlock *loopBodyEntryBB = entryBB;
  llvm::LLVMContext &C = loopBodyEntryBB->getContext();
  llvm::Function *F = loopBodyEntryBB->getParent();
  loopBodyEntryBB->setName("pregion.for.body");

  assert (exitBB->getTerminator()->getNumSuccessors() == 1);

  llvm::BasicBlock *oldExit = exitBB->getTerminator()->getSuccessor(0);

  llvm::BasicBlock *loopEndBB = 
    BasicBlock::Create(C, "pregion.for.end", F, exitBB);

  llvm::BasicBlock *forCondBB = 
    BasicBlock::Create(C, "pregion.for.cond", F, loopBodyEntryBB);

  llvm::BasicBlock *forIncBB = 
    BasicBlock::Create(C, "pregion.for.inc", F, forCondBB);

  DT->runOnFunction(*F);

  //  F->viewCFG();
  /* Fix the old edges jumping to the region to jump to the basic block
     that starts the created loop. Back edges should still point to the
     old basic block so we preserve the old loops. */
  BasicBlockVector preds;
  llvm::pred_iterator PI = 
    llvm::pred_begin(entryBB), 
    E = llvm::pred_end(entryBB);

  for (; PI != E; ++PI)
    {
      llvm::BasicBlock *bb = *PI;
      preds.push_back(bb);
    }    

  for (BasicBlockVector::iterator i = preds.begin();
       i != preds.end(); ++i)
    {
      llvm::BasicBlock *bb = *i;
      if (DT->dominates(loopBodyEntryBB, bb)) continue;
      bb->getTerminator()->replaceUsesOfWith(loopBodyEntryBB, forIncBB);
    }

  /* chain region body and loop exit */
  exitBB->getTerminator()->replaceUsesOfWith(oldExit, forIncBB);

  IRBuilder<> builder(forIncBB);
  builder.CreateBr(forCondBB);

  builder.SetInsertPoint(loopEndBB);
  builder.CreateBr(oldExit);

  builder.SetInsertPoint(forCondBB);
  llvm::Value *cmpResult = 
    builder.CreateICmpULT
    (builder.CreateLoad(localIdVar),
     (ConstantInt::get
      (IntegerType::get(C, size_t_width), 
       LocalSizeForDim)));
      
  builder.CreateCondBr(cmpResult, loopBodyEntryBB, loopEndBB);

  builder.SetInsertPoint(forIncBB->getTerminator());

  /* Create the iteration variable increment */
  builder.CreateStore
    (builder.CreateAdd
     (builder.CreateLoad(localIdVar),
      ConstantInt::get(IntegerType::get(C, size_t_width), 1)),
     localIdVar);


  //  F->viewCFG();
           
}

bool
WorkitemLoops::ProcessFunction(Function &F)
{
  Kernel *K = cast<Kernel> (&F);
  CheckLocalSize(K);

  ParallelRegion::ParallelRegionVector* original_parallel_regions =
    K->getParallelRegions(LI);

  llvm::Module *M = F.getParent();

  //  F.viewCFGOnly();

#ifdef DEBUG_WORK_ITEM_LOOPS
  for (ParallelRegion::ParallelRegionVector::iterator
           i = original_parallel_regions->begin(), 
           e = original_parallel_regions->end();
       i != e; ++i) 
  {
    ParallelRegion *region = (*i);
    std::cerr << "### PR: ";
    region->dumpNames();    
  }
#endif
  
  for (ParallelRegion::ParallelRegionVector::iterator
           i = original_parallel_regions->begin(), 
           e = original_parallel_regions->end();
       i != e; ++i) 
  {
    ParallelRegion *region = (*i);
    FixMultiRegionVariables(region);
  }

  for (ParallelRegion::ParallelRegionVector::iterator
           i = original_parallel_regions->begin(), 
           e = original_parallel_regions->end();
       i != e; ++i) 
  {
    ValueToValueMapTy reference_map;
    ParallelRegion *original = (*i);
    ParallelRegion *replica = 
      original->replicate(reference_map, ".wi_copy");
    original->insertPrologue(0, 0, 0);
    replica->chainAfter(original);
    replica->purge();

    if (LocalSizeZ > 1)
      CreateLoopAround(replica->entryBB(), replica->exitBB(), localIdZ, LocalSizeZ);
    if (LocalSizeY > 1)
      CreateLoopAround(replica->entryBB(), replica->exitBB(), localIdY, LocalSizeY);
    if (LocalSizeX > 1)
      CreateLoopAround(replica->entryBB(), replica->exitBB(), localIdX, LocalSizeX);
  }

  K->addLocalSizeInitCode(LocalSizeX, LocalSizeY, LocalSizeZ);

  //M->dump();
  //F.viewCFG();

  return true;
}

/*
 * Add context save/restore code to variables that are defined in 
 * the given region and are used outside the region.
 *
 * Each such variable gets a slot in the stack frame. The variable
 * is restored from the stack whenever its used.
 */
void
WorkitemLoops::FixMultiRegionVariables(ParallelRegion *region)
{
  InstructionIndex instructionsInRegion;
  InstructionVec instructionsToFix;

  /* Construct an index of the region's instructions so it's
     fast to figure out if the variable uses are all
     in the region. */
  for (BasicBlockVector::iterator i = region->begin();
       i != region->end(); ++i)
    {
      llvm::BasicBlock *bb = *i;
      for (llvm::BasicBlock::iterator instr = bb->begin();
           instr != bb->end(); ++instr) 
        {
          llvm::Instruction *instruction = instr;
          instructionsInRegion.insert(instruction);
        }
    }

  /* Find all the instructions that define new values and
     check if they need to be context saved. */
  for (BasicBlockVector::iterator i = region->begin();
       i != region->end(); ++i)
    {
      llvm::BasicBlock *bb = *i;
      for (llvm::BasicBlock::iterator instr = bb->begin();
           instr != bb->end(); ++instr) 
        {
          llvm::Instruction *instruction = instr;
          for (Instruction::use_iterator ui = instruction->use_begin(),
                 ue = instruction->use_end();
               ui != ue; ++ui) 
            {
              Instruction *user;
              if ((user = dyn_cast<Instruction> (*ui)) == NULL) continue;
              if (instructionsInRegion.find(user) == instructionsInRegion.end())
                {
                  instructionsToFix.push_back(instruction);
                  break;
                }
            }
        }
    }  

  /* Finally, fix the instructions. */
  for (InstructionVec::iterator i = instructionsToFix.begin();
       i != instructionsToFix.end(); ++i)
    {
#ifdef DEBUG_WORK_ITEM_LOOPS
      std::cerr << "### adding context/save restore for" << std::endl;
      (*i)->dump();
#endif      
      AddContextSaveRestore(*i, instructionsInRegion);
    }
}

/**
 * Adds context save/restore code for the value produced by the
 * given instruction.
 */
void
WorkitemLoops::AddContextSaveRestore
(llvm::Instruction *instruction, const InstructionIndex& instructionsInRegion)
{
  llvm::Module *M = instruction->getParent()->getParent()->getParent();
  
  /* Save the value to stack after its production. */ 
  IRBuilder<> builder(instruction->getParent()->getParent()->getEntryBlock().getFirstInsertionPt());

  llvm::Type *scalart = instruction->getType();
  llvm::Type *contextArrayType = 
    ArrayType::get(ArrayType::get(ArrayType::get(scalart, LocalSizeX), LocalSizeY), LocalSizeZ);

  /* Allocate the context data array for the variable. */
  llvm::Value *alloca = 
    builder.CreateAlloca(contextArrayType, 0, instruction->getName() + ".context_array");

  /* Save the produced variable to the array. */
  BasicBlock::iterator definition = instruction;
  builder.SetInsertPoint(++definition);
  std::vector<llvm::Value *> gepArgs;
  gepArgs.push_back(ConstantInt::get(IntegerType::get(instruction->getContext(), size_t_width), 0));
  gepArgs.push_back(builder.CreateLoad(localIdZ));
  gepArgs.push_back(builder.CreateLoad(localIdY));
  gepArgs.push_back(builder.CreateLoad(localIdX)); 
  llvm::Value *theStore = builder.CreateStore(instruction, builder.CreateGEP(alloca, gepArgs));

  InstructionVec uses;
  /* Restore the produced variable before each use outside the region. */

  /* Find out the uses to fix first as fixing them causes invalidates
     the iterator. */
  for (Instruction::use_iterator ui = instruction->use_begin(),
         ue = instruction->use_end();
       ui != ue; ++ui) 
    {
      Instruction *user;
      if ((user = dyn_cast<Instruction> (*ui)) == NULL) continue;
      if (user == theStore) continue;
      if (instructionsInRegion.find(user) != instructionsInRegion.end()) continue;
      uses.push_back(user);
    }

  for (InstructionVec::iterator i = uses.begin(); i != uses.end(); ++i)
    {
      Instruction *user = *i;
      builder.SetInsertPoint(user);

      std::vector<llvm::Value *> gepArgs;
      gepArgs.push_back(ConstantInt::get(IntegerType::get(instruction->getContext(), size_t_width), 0));
      gepArgs.push_back(builder.CreateLoad(localIdZ));
      gepArgs.push_back(builder.CreateLoad(localIdY));
      gepArgs.push_back(builder.CreateLoad(localIdX)); 
          
      llvm::Value *loadedValue = builder.CreateLoad(builder.CreateGEP(alloca, gepArgs));
      user->replaceUsesOfWith(instruction, loadedValue);
    }
}
