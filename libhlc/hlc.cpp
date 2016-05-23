#include "hlc.hh"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/RegionPass.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/CodeGen/LinkAllAsmWriterComponents.h"
#include "llvm/CodeGen/LinkAllCodegenComponents.h"
#include "llvm/CodeGen/MIRParser/MIRParser.h"
#include "llvm/InitializePasses.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/LegacyPassNameParser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/Verifier.h"
#include "llvm/LinkAllIR.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Linker/Linker.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Pass.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace libHLC
{


    static llvm::LLVMContext *TheContext = nullptr;

    bool DisableInline = false;
    bool UnitAtATime = true;
    bool DisableLoopVectorization = false;
    bool DisableSLPVectorization = false;
    bool StripDebug = false;
    bool DisableOptimizations = false;
    bool DisableSimplifyLibCalls = false;

    static const std::string MArch = "amdgcn"; // AMD Graphics Core Next

    // ModuleRef impl
    ModuleRef::ModuleRef(Module * module) : M(module) { };

    ModuleRef::operator bool () const
    {
        return M != nullptr;
    }

    Module * ModuleRef::getModule()
    {
        return M;
    }

    void ModuleRef::destroy()
    {
        delete M;
        M = nullptr;
    }

    std::string ModuleRef::to_string()
    {
        std::string buf;
        raw_string_ostream os(buf);
        M->print(os, nullptr);
        os.flush();
        return buf;
    }

    ModuleRef * ModuleRef::parseAssembly(const char* Asm)
    {
        SMDiagnostic SM;
        Module* M = parseAssemblyString(Asm, SM, *TheContext).release();
        if (!M) return nullptr;
        return new ModuleRef(M);
    }

    ModuleRef * ModuleRef::parseBitcode(const char *Bitcode, size_t Len)
    {
        auto buf = MemoryBuffer::getMemBuffer(StringRef(Bitcode, Len),
                                              "", false);

        MemoryBufferRef mbref = buf->getMemBufferRef();
        ErrorOr<std::unique_ptr<Module>> ModuleOrErr =
                                          parseBitcodeFile(mbref, *TheContext);

        if (std::error_code EC = ModuleOrErr.getError())
        {
            puts(EC.message().c_str());
            return nullptr;
        }

        std::unique_ptr<Module> mod (std::move(ModuleOrErr.get()));

        mod->materializeAll();

        ModuleRef * mref = new ModuleRef(mod.release());
        return mref;
    }

    CodeGenOpt::Level GetCodeGenOptLevel(int OptLevel)
    {
        switch (OptLevel)
        {
            case 1:
                return CodeGenOpt::Less;
            case 2:
                return CodeGenOpt::Default;
            case 3:
                return CodeGenOpt::Aggressive;
            default:
                return CodeGenOpt::None;
        }
    }

    // The following function combines initialisation code from opt and llc
    // tools as found in the llvm source tree, here:
    // https://github.com/llvm-mirror/llvm/blob/master/tools/opt/opt.cpp
    // and here:
    // https://github.com/llvm-mirror/llvm/blob/master/tools/llc/llc.cpp
    void Initialize()
    {
        using namespace llvm;

        if ( TheContext != nullptr )
        {
            // Already initialized
            return;
        }

        sys::PrintStackTraceOnErrorSignal();
        EnablePrettyStackTrace();

        // Enable debug stream buffering.
        EnableDebugBuffering = true;

        // this has thread safety issues, there's no global context anymore
        // each thread really ought to have its own.
        LLVMContext * Context = new LLVMContext();
        TheContext = Context;

        // Initialize targets

        // FROM OPT
        InitializeAllTargets();
        InitializeAllTargetMCs();
        InitializeAllAsmPrinters();

        // FROM LLC
        InitializeAllAsmParsers();

        // Initialize passes
        PassRegistry &Registry = *PassRegistry::getPassRegistry();


        // FROM OPT
        initializeCore(Registry);
        initializeScalarOpts(Registry);
        initializeObjCARCOpts(Registry);
        initializeVectorization(Registry);
        initializeIPO(Registry);
        initializeAnalysis(Registry);
        initializeTransformUtils(Registry);
        initializeInstCombine(Registry);
        initializeInstrumentation(Registry);
        initializeTarget(Registry);
        // For codegen passes, only passes that do IR to IR transformation are
        // supported.
        initializeCodeGenPreparePass(Registry);
        initializeAtomicExpandPass(Registry);
        initializeRewriteSymbolsPass(Registry);
        initializeWinEHPreparePass(Registry);
        initializeDwarfEHPreparePass(Registry);
        initializeSafeStackPass(Registry);
        initializeSjLjEHPreparePass(Registry);
        initializePreISelIntrinsicLoweringPass(Registry);


        // FROM LLC
        initializeCodeGen(Registry);
        initializeLoopStrengthReducePass(Registry);
        initializeLowerIntrinsicsPass(Registry);
        initializeUnreachableBlockElimPass(Registry);

    }

    void Finalize()
    {
        using namespace llvm;

        llvm_shutdown();
    }


    // The following section is adapted from opt.cpp from the LLVM source tree.
    // Original code is here:
    // https://github.com/llvm-mirror/llvm/blob/master/tools/opt/opt.cpp

    // --- Start OPT section ---

    static inline void addPass(legacy::PassManagerBase &PM, Pass *P)
    {
        // Add the pass to the pass manager...
        PM.add(P);

        // If we are verifying all of the intermediate steps, add the verifier...
        PM.add(createVerifierPass());
    }

    /// This routine adds optimization passes based on selected optimization level,
    /// OptLevel.
    static void AddOptimizationPasses(legacy::PassManagerBase &MPM,
                                      legacy::FunctionPassManager &FPM,
                                      unsigned OptLevel, unsigned SizeLevel)
    {
        FPM.add(createVerifierPass()); // Verify that input is correct

        PassManagerBuilder Builder;
        Builder.OptLevel = OptLevel;
        Builder.SizeLevel = SizeLevel;

        if (DisableInline)
        {
            // No inlining pass
        }
        else if (OptLevel > 1)
        {
            Builder.Inliner = createFunctionInliningPass(OptLevel, SizeLevel);
        }
        else
        {
            Builder.Inliner = createAlwaysInlinerPass();
        }
        Builder.DisableUnitAtATime = !UnitAtATime;
        Builder.DisableUnrollLoops = OptLevel == 0;

        // This is final, unless there is a #pragma vectorize enable
        if (DisableLoopVectorization)
            Builder.LoopVectorize = false;
        // If option wasn't forced via cmd line (-vectorize-loops, -loop-vectorize)
        else if (!Builder.LoopVectorize)
            Builder.LoopVectorize = OptLevel > 1 && SizeLevel < 2;

        // When #pragma vectorize is on for SLP, do the same as above
        Builder.SLPVectorize =
            DisableSLPVectorization ? false : OptLevel > 1 && SizeLevel < 2;

        Builder.populateFunctionPassManager(FPM);
        Builder.populateModulePassManager(MPM);
    }

    static void AddStandardLinkPasses(legacy::PassManagerBase &PM)
    {
        PassManagerBuilder Builder;
        Builder.VerifyInput = true;
        if (DisableOptimizations)
            Builder.OptLevel = 0;

        if (!DisableInline)
            Builder.Inliner = createFunctionInliningPass();
        Builder.populateLTOPassManager(PM);
    }

    // Returns the TargetMachine instance or zero if no triple is provided.
    static TargetMachine* GetTargetMachine(Triple TheTriple, StringRef CPUStr,
                                           StringRef FeaturesStr,
                                           const TargetOptions &Options,
                                           int OptLevel
                                          )
    {
        std::string Error;
        const Target *TheTarget = TargetRegistry::lookupTarget(MArch, TheTriple,
                                  Error);
        // Some modules don't specify a triple, and this is okay.
        if (!TheTarget)
        {
            return nullptr;
        }

        return TheTarget->createTargetMachine(TheTriple.getTriple(),
                                              CPUStr, FeaturesStr, Options,
                                              getRelocModel(), CMModel,
                                              GetCodeGenOptLevel(OptLevel));
    }

    void Optimize(llvm::Module * M, int OptLevel, int SizeLevel, int Verify)
    {

        bool OptLevelO1, OptLevelO2, OptLevelO3, StandardLinkOpts;
        switch(OptLevel)
        {
            case 0:
                break;
            case 1:
                OptLevelO1 = true;
                break;
            case 2:
                OptLevelO2 = true;
                break;
            case 3:
                OptLevelO3 = true;
                break;
        }

        if(OptLevel > 0)
        {
            StandardLinkOpts = true;
        }

        // Strip debug info before running the verifier.
        if (StripDebug)
            StripDebugInfo(*M);

        // Immediately run the verifier to catch any problems before starting up the
        // pass pipelines.  Otherwise we can crash on broken code during
        // doInitialization().
        if(verifyModule(*M, &errs()))
        {
            errs() << "error: input module is broken!\n";
            exit(1);
        }


        M->setTargetTriple(Triple::normalize("amdgcn--amdhsa"));


        Triple ModuleTriple(M->getTargetTriple());
        std::string CPUStr="fiji", FeaturesStr="";
        TargetMachine *Machine = nullptr;
        TargetOptions Options;

        if (ModuleTriple.getArch())
        {
            Machine = GetTargetMachine(ModuleTriple, CPUStr, FeaturesStr, Options, OptLevel);
        }

        std::unique_ptr<TargetMachine> TM(Machine);

        // Override function attributes based on CPUStr, FeaturesStr, and command line
        // flags.
        setFunctionAttributes(CPUStr, FeaturesStr, *M);

        // Create a PassManager to hold and optimize the collection of passes we are
        // about to build.
        legacy::PassManager Passes;

        // Add an appropriate TargetLibraryInfo pass for the module's triple.
        TargetLibraryInfoImpl TLII(ModuleTriple);

        // switch off libcall simplication, transforming loops to
        // system calls is not supported
        TLII.disableAllFunctions();
        Passes.add(new TargetLibraryInfoWrapperPass(TLII));

        // Add an appropriate DataLayout instance for this module.
        const DataLayout &DL = M->getDataLayout();
        if (DL.isDefault())
        {
            M->setDataLayout("");
        }

        // Add internal analysis passes from the target machine.
        Passes.add(createTargetTransformInfoWrapperPass(TM ? TM->getTargetIRAnalysis()
                   : TargetIRAnalysis()));

        std::unique_ptr<legacy::FunctionPassManager> FPasses;
        if (OptLevelO1 || OptLevelO2 || OptLevelO3)
        {
            FPasses.reset(new legacy::FunctionPassManager(M));
            FPasses->add(createTargetTransformInfoWrapperPass(
                             TM ? TM->getTargetIRAnalysis() : TargetIRAnalysis()));
        }

        if (StandardLinkOpts)
            AddStandardLinkPasses(Passes);

        // Apply optimisation passes
        if (OptLevelO1)
            AddOptimizationPasses(Passes, *FPasses, 1, 0);

        if (OptLevelO2)
            AddOptimizationPasses(Passes, *FPasses, 2, 0);

        if (OptLevelO3)
            AddOptimizationPasses(Passes, *FPasses, 3, 0);

        if (OptLevelO1 || OptLevelO2 || OptLevelO3)
        {
            FPasses->doInitialization();
            for (Function &F : *M)
                FPasses->run(F);
            FPasses->doFinalization();
        }

        // Check that the module is well formed on completion of optimization
        Passes.add(createVerifierPass());

        // Now that we have all of the passes ready, run them.
        Passes.run(*M);
    }


    // --- END OPT section ---



    // The following section is adapted from llc.cpp from the LLVM source tree.
    // Original code is here:
    // https://github.com/llvm-mirror/llvm/blob/master/tools/llc/llc.cpp

    // --- START LLC section ---

    int CompileModule(std::unique_ptr<Module> mod, raw_string_ostream &os, bool emitBRIG,
                      int OptLevel)
    {
        // Load the module to be compiled...
        SMDiagnostic Err;

        Triple TheTriple;// = Triple(mod->getTargetTriple());

        TheTriple = Triple(Triple::normalize("amdgcn--amdhsa"));

        // Get the target specific parser.
        std::string Error;
        const Target *TheTarget = TargetRegistry::lookupTarget(MArch, TheTriple,
                                  Error);

        if (!TheTarget)
        {
            errs() << Error;
            return 0;
        }

        // Package up features to be passed to target/subtarget
        std::string CPUStr = "fiji", FeaturesStr = "+promote-alloca,+fp64-denormals,+flat-for-global,";

        CodeGenOpt::Level OLvl = CodeGenOpt::Default;
        switch (OptLevel)
        {
            case 0:
                OLvl = CodeGenOpt::None;
                break;
            case 1:
                OLvl = CodeGenOpt::Less;
                break;
            case 2:
                OLvl = CodeGenOpt::Default;
                break;
            case 3:
                OLvl = CodeGenOpt::Aggressive;
                break;
        }

        TargetOptions Options = InitTargetOptionsFromCodeGenFlags();
        Options.MCOptions.AsmVerbose = true;

        std::unique_ptr<TargetMachine> Target(
            TheTarget->createTargetMachine(TheTriple.getTriple(), CPUStr, FeaturesStr,
                                           Options, getRelocModel(), CMModel, OLvl));


        assert(Target && "Could not allocate target machine!");
        assert(mod && "Should have exited if we didn't have a module!");

        if (FloatABIForCalls != FloatABI::Default)
            Options.FloatABIType = FloatABIForCalls;

        // Build up all of the passes that we want to do to the module.
        legacy::PassManager PM;

        // Add an appropriate TargetLibraryInfo pass for the module's triple.
        TargetLibraryInfoImpl TLII(Triple(mod->getTargetTriple()));

        // The -disable-simplify-libcalls flag actually disables all builtin optzns.
        TLII.disableAllFunctions();

        PM.add(new TargetLibraryInfoWrapperPass(TLII));

        // Add the target data from the target machine, if it exists, or the module./
        mod->setDataLayout(Target->createDataLayout());

        setFunctionAttributes(CPUStr, FeaturesStr, *mod);

        auto FileType = (emitBRIG
                         ? TargetMachine::CGFT_ObjectFile
                         : TargetMachine::CGFT_AssemblyFile);

        {
            // new scope
            buffer_ostream BOS(os);

            // Ask the target to add backend passes as necessary.
            bool Verify = true;
            if (Target->addPassesToEmitFile(PM, BOS, FileType, Verify))
            {
                errs() << "target does not support generation of this"
                       << " file type!\n";
                return 1;
            }
            PM.run(*mod);
        }
        return 0;
    }

    // --- END LLC section ---

} // end libHLC namespace

extern "C" {

    using namespace libHLC;

    typedef struct OpaqueModule* llvm_module_ptr;

    void HLC_Initialize()
    {
        Initialize();
    }

    void HLC_Finalize()
    {
        Finalize();
    }


    char* HLC_CreateString(const char *str)
    {
        return strdup(str);
    }

    void HLC_DisposeString(char *str)
    {
        free(str);
    }

    ModuleRef* HLC_ParseModule(const char *Asm)
    {
        return ModuleRef::parseAssembly(Asm);
    }

    ModuleRef* HLC_ParseBitcode(const char *Asm, size_t Len)
    {
        ModuleRef * mref = ModuleRef::parseBitcode(Asm, Len);
        return mref;
    }

    void HLC_ModulePrint(ModuleRef *M, char **output)
    {
        *output = HLC_CreateString(M->to_string().c_str());
    }

    void HLC_ModuleDestroy(ModuleRef *M)
    {
        M->destroy();
        delete M;
    }

    int HLC_ModuleOptimize(ModuleRef *M, int OptLevel, int SizeLevel, int Verify)
    {
        if (OptLevel < 0 && OptLevel > 3) return 0;
        if (SizeLevel < 0 && SizeLevel > 2) return 0;
        Module * mref = M->getModule();
        Optimize(mref, OptLevel, SizeLevel, Verify);
        return 1;
    }


    int HLC_ModuleLinkIn(ModuleRef * Dst, ModuleRef * Src)
    {
        const Module * ref = Src->getModule();
        std::unique_ptr<Module> sM = llvm::CloneModule (ref);

        if(llvm::verifyModule(*Dst->getModule(), nullptr))
        {
            return 0;
        }
        if(llvm::verifyModule(*Src->getModule(), nullptr))
        {
            return 0;
        }
        int status = llvm::Linker::linkModules(*Dst->getModule(), std::move(sM), 0);
        return !status;
    }


    int HLC_ModuleEmitHSAIL(ModuleRef *M, int OptLevel, char **output)
    {
        const Module * ref = M->getModule();
        std::unique_ptr<Module> sM = llvm::CloneModule (ref);

        if (OptLevel < 0 && OptLevel > 3) return 0;
        // Compile
        std::string buf;
        raw_string_ostream os(buf);
        int status = CompileModule(std::move(sM), os, false, OptLevel);
        if(status) return 0;
        // Write output
        os.flush();
        *output = HLC_CreateString(buf.c_str());
        return 1;
    }

    size_t HLC_ModuleEmitBRIG(ModuleRef *M, int OptLevel, char **output)
    {
        const Module * ref = M->getModule();
        std::unique_ptr<Module> sM = llvm::CloneModule (ref);

        if (OptLevel < 0 && OptLevel > 3) return 0;
        // Compile
        std::string buf;
        raw_string_ostream os(buf);
        int status  = CompileModule(std::move(sM), os, true, OptLevel);
        if(status) return 0;
        // Write output
        os.flush();
        *output = (char*)malloc(buf.size());
        memcpy(*output, buf.data(), buf.size());
        return buf.size();
    }

    void HLC_SetCommandLineOption(int argc, const char * const * argv)
    {
        llvm::cl::ParseCommandLineOptions(argc, argv, nullptr);
    }

} // end extern "C"
