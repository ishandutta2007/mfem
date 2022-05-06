// Copyright (c) 2010-2022, Lawrence Livermore National Security, LLC. Produced
// at the Lawrence Livermore National Laboratory. All Rights reserved. See files
// LICENSE and NOTICE for details. LLNL-CODE-806117.
//
// This file is part of the MFEM library. For more information and source code
// availability visit https://mfem.org.
//
// MFEM is free software; you can redistribute it and/or modify it under the
// terms of the BSD-3 license. We welcome feedback and contributions, see file
// CONTRIBUTING.md for details.

#include "../../config/config.hpp"

#ifdef MFEM_USE_JIT

#include <cstdlib> // for exit, system

#include <chrono>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <dlfcn.h> // for dlopen/dlsym, not available on Windows
#include <signal.h> // for signals
#include <unistd.h> // for fork
#include <sys/wait.h> // for waitpid

#if !(defined(__linux__) || defined(__APPLE__))
#error mmap(2) implementation as defined in POSIX.1-2001 not supported.
#else
#include <sys/mman.h> // for memory map
#endif

#include "../communication.hpp" // pulls mpi.h
#include "../globals.hpp" // needed when !MFEM_USE_MPI
#include "../error.hpp" // for MFEM_CONTRACT_VAR
#include "jit.hpp"

namespace mfem
{

namespace jit_internal
{

struct MPI
{
   // Return the MPI world rank
   static int Rank()
   {
      int world_rank = 0;
#ifdef MFEM_USE_MPI
      if (Mpi::IsInitialized()) { world_rank = Mpi::WorldRank(); }
#endif
      return world_rank;
   }

   // Return true if MPI has been initialized
   static bool IsInitialized()
   {
#ifndef MFEM_USE_MPI
      return false;
#else
      return Mpi::IsInitialized();
#endif
   }

   // Return true if the rank in world rank is zero
   static bool Root() { return Rank() == 0; }

   // Do a MPI barrier and status reduction if MPI has been initialized
   static void Sync(bool status = EXIT_SUCCESS)
   {
#ifdef MFEM_USE_MPI
      if (Mpi::IsInitialized())
      {
         MPI_Allreduce(MPI_IN_PLACE, &status, 1, MPI_C_BOOL, MPI_LOR, MPI_COMM_WORLD);
         MFEM_VERIFY(status == EXIT_SUCCESS, "[JIT] Synchronization error!");
      }
#else
      MFEM_CONTRACT_VAR(status);
#endif
   }
};

struct JIT // System singleton object
{
   pid_t pid; // of child process
   int *s_ack; // shared status, should be large enough to store one MPI rank
   char *s_mem; // shared memory to store the command for the system call
   uintptr_t size; // of the s_mem shared memory

   struct Command
   {
      std::ostringstream cmd {};
      Command& operator<<(const char *c) { cmd << c << ' '; return *this; }
      Command& operator<<(const std::string &s) { return *this << s.c_str(); }
      operator const char *()
      {
         std::ostringstream cmd_mv = std::move(cmd);
         static thread_local std::string sl_cmd;
         sl_cmd = cmd_mv.str();
         cmd.clear(); cmd.str(""); // flush for next command
         return sl_cmd.c_str();
      }
   } command;

   template <typename OP> static void Ack(const int xx)
   {
      while (OP()(*Ack(), xx)) // equal_to or not_equal_to OP
      { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
   }
   static constexpr int ACK = ~0, CALL = 0x3243F6A8, EXIT = 0x9e3779b9;
   static int Read() { return *Ack(); }
   static void Acknowledge() { Write(ACK); }
   static void Write(const int xx) { *Ack() = xx; }
   static void Send(const int xx) { AckNE(*Ack() = xx); } // block until acknowledged
   static void Wait(const bool eq = true) { eq ? AckEQ() : AckNE(); }
   static void AckEQ(const int xx = ACK) { Ack<std::equal_to<int>>(xx); }
   static void AckNE(const int xx = ACK) { Ack<std::not_equal_to<int>>(xx); }

   static bool IsCall() { return Read() == CALL; }
   static bool IsExit() { return Read() == EXIT; }
   static bool IsAck() { return Read() == ACK; }

   // Ask the parent to launch a system call
   static int Call(const char *command = Command())
   {
      MFEM_VERIFY(MPI::Root(), "[JIT] Only MPI root should launch commands!");
      MFEM_WARNING("\033[33m" << command << "\033[m");
      // In serial mode, just call std::system
      if (!MPI::IsInitialized()) { return std::system(command); }
      // Otherwise, write the command to the child process
      MFEM_VERIFY((1+std::strlen(command))<Size(), "[JIT] Command length error!");
      std::memcpy(Mem(), command, std::strlen(command) + 1);
      Send(CALL); // call std::system through the child process
      Wait(false); // wait for the acknowledgment
      return EXIT_SUCCESS;
   }

   static JIT singleton;
   static JIT& Get() { return singleton; }

   static pid_t& Pid() { return Get().pid; }
   static int* Ack() { return Get().s_ack; }
   static char* Mem() { return Get().s_mem; }
   static uintptr_t Size() { return Get().size; }
   static Command& Command() { return Get().command; }

   static void Handler(int signum)
   {
      if (Pid()==0) { assert(false); } // child
      MFEM_CONTRACT_VAR(signum);
      ::kill(Pid(), SIGKILL);
      while ((Pid() = ::wait(nullptr)) > 0);
      std::exit(EXIT_FAILURE);
   }

   static void Init(int *argc, char ***argv)
   {
      constexpr int prot = PROT_READ | PROT_WRITE;
      constexpr int flags = MAP_SHARED | MAP_ANONYMOUS;
      Get().size = (uintptr_t) sysconf(_SC_PAGE_SIZE);
      Get().s_ack = (int*) ::mmap(nullptr, sizeof(int), prot, flags, -1, 0);
      Get().s_mem = (char*) ::mmap(nullptr, Get().size, prot, flags, -1, 0);
      Write(ACK); // initialize state
      ::signal(SIGINT,  Handler); // interrupt
      ::signal(SIGQUIT, Handler); // quit
      ::signal(SIGABRT, Handler); // abort
      ::signal(SIGKILL, Handler); // kill
      ::signal(SIGFPE,  Handler); // floating point exception

      if ((Pid() = ::fork()) != 0)
      {
#ifdef MFEM_USE_MPI
         ::MPI_Init(argc, argv);
#else
         MFEM_CONTRACT_VAR(argc);
         MFEM_CONTRACT_VAR(argv);
#endif
         Write(MPI::Rank()); // inform the child about the rank
         Wait(false); // wait for the child to acknowledge
      }
      else { Child(); }
   }

   static void Child()
   {
      MFEM_VERIFY(IsAck(), "[JIT] Child process initialization error!");
      Wait(); // wait for parent's rank
      const int rank = Read();
      Acknowledge();
      if (rank == 0) // only root is kept for work
      {
         std::thread([&]()
         {
            while (true)
            {
               Wait(); // waiting for the root to wake us
               if (IsCall()) { if (std::system(Mem())) { return; } }
               if (IsExit()) { return;}
               Acknowledge();
            }
         }).join();
      }
      std::exit(EXIT_SUCCESS);
   }

   static void Finalize()
   {
      MFEM_VERIFY(IsAck(), "[JIT] Finalize acknowledgment error!");
      int status;
      Send(EXIT);
      ::waitpid(0, &status, WUNTRACED | WCONTINUED); // wait for any child
      MFEM_VERIFY(status == 0, "[JIT] Error with the compiler thread");
      if (::munmap(Mem(), Size()) != 0 || // release shared memory
          ::munmap(Ack(), sizeof(int)) != 0)
      { MFEM_ABORT("[JIT] Finalize memory error!"); }
   }

   struct HostOptions
   {
      virtual std::string Device() { return ""; }
      virtual std::string Compiler() { return ""; }
      virtual std::string Linker() { return "-Wl,"; }
   };

   struct DeviceOptions: HostOptions
   {
      std::string Device() override { return "--device-c"; }
      std::string Compiler() override { return "-Xcompiler="; }
      std::string Linker() override { return "-Xlinker="; }
   };

   struct Linux
   {
      virtual std::string Prefix() { return Xlinker() + "--whole-archive"; }
      virtual std::string Postfix() { return Xlinker() + "--no-whole-archive"; }
      virtual std::string Backup() { return "--backup=none"; }
   };

   struct Apple: public Linux
   {
      std::string Prefix() override { return "-all_load"; }
      std::string Postfix() override { return ""; }
      std::string Backup() override { return ""; }
   };

#if !(defined(MFEM_USE_CUDA) || defined(MFEM_USE_HIP))
   HostOptions options;
#else
   DeviceOptions options;
#endif

#ifdef __APPLE__
   Apple archive;
#else
   Linux archive;
#endif

   static const char *ar() { return "libmjit.a"; }
   static const char *so() { return "./libmjit.so"; }
   static std::string CXX() { return MFEM_JIT_CXX; }
   static std::string FLAGS() { return MFEM_JIT_BUILD_FLAGS; }
   static std::string Xdevice() { return Get().options.Device(); }
   static std::string Xlinker() { return Get().options.Linker(); }
   static std::string Xcompiler() { return Get().options.Compiler(); }
   static std::string ARprefix() { return Get().archive.Prefix();  }
   static std::string ARpostfix() { return Get().archive.Postfix(); }
   static std::string ARbackup() { return Get().archive.Backup(); }
   static void *DLopen(const char *path) { return ::dlopen(path, RTLD_LAZY|RTLD_LOCAL); }

   static void* Lookup(const size_t hash, const char *source, const char *symbol)
   {
      void *handle = // try to open libmjit.so first
         std::fstream(so()) ? DLopen(so()) : nullptr;

      // if libmjit.so was not found, try libmjit.a
      if (!handle && std::fstream(ar()))
      {
         int status = EXIT_SUCCESS;
         if (MPI::Root())
         {
            Command() << CXX() << "-shared" << "-o" << so()
                      << ARprefix() << ar() << ARpostfix()
                      << Xlinker() + "-rpath,.";
            status = Call();
         }
         MPI::Sync(status);
         handle = DLopen(so());
         MFEM_VERIFY(handle, "[JIT] Error " << so() << " from " << ar());
      }

      auto RootCompile = [&]() // Compilation done by the MPI root rank only
      {
         auto FileNameFromHash64 = [](const size_t h, const char *ext)
         {
            std::stringstream ss {};
            ss << 'k' << std::setfill('0') << std::setw(16)
               << std::hex << (h|0) << std::dec << ext;
            return ss.str();
         };
         auto cc = FileNameFromHash64(hash, ".cc"); // input file
         auto co = FileNameFromHash64(hash, ".co"); // output object

         // Write kernel source into input file
         std::ofstream input_src_file(cc);
         MFEM_VERIFY(input_src_file.good(), "[JIT] Input file error!");
         input_src_file << source;
         input_src_file.close();

         // Compilation: cc => co
         Command() << CXX() << FLAGS() << Xdevice()
                   << Xcompiler() + "-fPIC" << Xcompiler() + "-pipe"
                   << Xcompiler() + "-Wno-unused-variable"
                   << "-c" << "-o" << co << cc;
         if (Call()) { return EXIT_FAILURE; }
         std::remove(cc.c_str());
         // Update archive: ar += co
         Command() << "ar -rv" << ar() << co;
         if (Call()) { return EXIT_FAILURE; }
         std::remove(co.c_str());
         // Create shared library: new (ar + symbol), used afterward
         Command() << CXX() << "-shared" << "-o" << symbol
                   << ARprefix() << ar() << ARpostfix();
         if (Call()) { return EXIT_FAILURE; }
         // Update shared cache library: new (ar + symbol) => LIB_SO
         Command() << "install" << "-v" << ARbackup() << symbol << so();
         if (Call()) { return EXIT_FAILURE; }
         return EXIT_SUCCESS;
      }; // RootCompile

      auto WorldCompile = [&]() // only root compiles
      {
         const bool status = MPI::Root() ? RootCompile() : EXIT_SUCCESS;
         MPI::Sync(status); // all ranks verify the status
         handle = DLopen(symbol); // opens (ar + symbol)
         MPI::Sync();
         MFEM_VERIFY(handle, "[JIT] Error creating handle:" << ::dlerror());
      }; // WorldCompile

      // no caches => launch compilation
      if (!handle) { WorldCompile(); }
      MFEM_VERIFY(handle, "[JIT] No handle could be created!");

      void *kernel = ::dlsym(handle, symbol); // symbol lookup
      // no symbol => launch compilation & update kernel symbol
      if (!kernel) { WorldCompile(); kernel = ::dlsym(handle, symbol); }
      MFEM_VERIFY(kernel, "[JIT] No kernel could be found!");

      // remove temporary shared (ar + symbol), the cache will be used afterward
      std::remove(symbol);
      return kernel;
   }
};
JIT JIT::singleton {}; // Initialize the unique System context.

} // namespace internal

using namespace jit_internal;

void Jit::Init(int *argc, char ***argv) { if (MPI::Root()) JIT::Init(argc, argv); }

void Jit::Finalize() { if (MPI::Root()) JIT::Finalize(); }

void* Jit::Lookup(const size_t hash, const char *source, const char *symbol)
{
   return JIT::Lookup(hash, source, symbol);
}

} // namespace mfem

#endif // MFEM_USE_JIT