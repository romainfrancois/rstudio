/*
 * RCompilationDatabase.cpp
 *
 * Copyright (C) 2009-12 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "RCompilationDatabase.hpp"

#include <algorithm>

#include <boost/format.hpp>
#include <boost/regex.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/trim_all.hpp>

#include <core/PerformanceTimer.hpp>
#include <core/FileSerializer.hpp>

#include <core/r_util/RToolsInfo.hpp>

#include <core/system/ProcessArgs.hpp>

#include <core/r_util/RPackageInfo.hpp>

#include <r/RExec.hpp>

#include <session/projects/SessionProjects.hpp>
#include <session/SessionModuleContext.hpp>
#include <session/SessionUserSettings.hpp>

#include <core/libclang/LibClang.hpp>

using namespace core ;
using namespace core::libclang;

namespace session {
namespace modules { 
namespace clang {

namespace {

LibClang& clang()
{
   return libclang::clang();
}


std::string sourceCppHash(const core::FilePath& srcPath)
{
   // read file
   std::string contents;
   Error error = core::readStringFromFile(srcPath, &contents);
   if (error)
   {
      LOG_ERROR(error);
      return std::string();
   }

   // we use Rcpp::sourceCpp with dryRun to determine the compiler command
   // line, as well as generate and use Rcpp precompiled headers. For this
   // reason we need to restrict sourceCpp support to straight Rcpp --
   // this code filters out files that use Rcpp11 (note that packages using
   // Rcpp11 do however work correctly)
   boost::regex reRcpp11("#include\\s+<Rcpp11");
   if (boost::regex_search(contents, reRcpp11))
      return std::string();

   // hash to return
   std::string hash;

   // find dependency attributes
   boost::regex re(
     "^\\s*//\\s*\\[\\[Rcpp::(\\w+)(\\(.*?\\))?\\]\\]\\s*$");
   boost::sregex_token_iterator it(contents.begin(), contents.end(), re, 0);
   boost::sregex_token_iterator end;
   for ( ; it != end; ++it)
   {
      std::string attrib = *it;
      boost::algorithm::trim_all(attrib);
      hash.append(attrib);
   }

   // if the hash is empty we can still quality with an explicit
   // include of Rcpp
   if (hash.empty())
   {
      boost::regex reRcpp("#include\\s+<Rcpp");
      if (boost::regex_search(contents, reRcpp))
         hash = "Rcpp";
   }

   // return hash
   return hash;
}

std::vector<std::string> extractCompileArgs(const std::string& line)
{
   std::vector<std::string> compileArgs;

   // find arguments libclang might care about
   boost::regex re("[ \\t]-(?:[IDif]|std)(?:\\\"[^\\\"]+\\\"|[^ ]+)");
   boost::sregex_token_iterator it(line.begin(), line.end(), re, 0);
   boost::sregex_token_iterator end;
   for ( ; it != end; ++it)
   {
      // remove quotes and add it to the compile args
      std::string arg = *it;
      boost::algorithm::trim_all(arg);
      boost::algorithm::replace_all(arg, "\"", "");
      compileArgs.push_back(arg);
   }

   return compileArgs;
}

std::string extractStdArg(const std::vector<std::string>& args)
{
   BOOST_FOREACH(const std::string& arg, args)
   {
      if (boost::algorithm::starts_with(arg, "-std="))
         return arg;
   }

   return std::string();
}

std::string buildFileHash(const FilePath& filePath)
{
   if (filePath.exists())
   {
      std::ostringstream ostr;
      ostr << filePath.lastWriteTime();
      return ostr.str();
   }
   else
   {
      return std::string();
   }
}

std::string packageBuildFileHash()
{
   std::ostringstream ostr;
   FilePath buildPath = projects::projectContext().buildTargetPath();
   ostr << buildFileHash(buildPath.childPath("DESCRIPTION"));
   FilePath srcPath = buildPath.childPath("src");
   if (srcPath.exists())
   {
      ostr << buildFileHash(srcPath.childPath("Makevars"));
      ostr << buildFileHash(srcPath.childPath("Makevars.win"));
   }
   return ostr.str();
}

core::system::Options compilationEnvironment()
{
   // rtools on windows
   core::system::Options env;
   core::system::environment(&env);
#if defined(_WIN32)
   std::vector<std::string> rtoolsArgs = rToolsArgs();
   std::copy(rtoolsArgs.begin(),
             rtoolsArgs.end(),
             std::back_inserter(compileArgs));

   std::string warning;
   module_context::addRtoolsToPathIfNecessary(&env, &warning);
#endif
   return env;
}

std::vector<std::string> parseCompilationResults(const FilePath& srcFile,
                                                 const std::string& results)
{
   // compile args to return
   std::vector<std::string> compileArgs;

   // break into lines
   std::vector<std::string> lines;
   boost::algorithm::split(lines, results,
                           boost::algorithm::is_any_of("\r\n"));


   // find the line with the compilation and add it's args
   std::string compile = "-c " + srcFile.filename() + " -o " + srcFile.stem();
   BOOST_FOREACH(const std::string& line, lines)
   {
      if (line.find(compile) != std::string::npos)
      {
         std::vector<std::string> args = extractCompileArgs(line);
         std::copy(args.begin(), args.end(), std::back_inserter(compileArgs));
      }
   }

   // return the args
   return compileArgs;
}

std::string packagePCH(const std::string& linkingTo)
{
   std::string pch;
   r::exec::RFunction func(".rs.packagePCH", linkingTo);
   Error error = func.call(&pch);
   if (error)
   {
      error.addProperty("linking-to", linkingTo);
      LOG_ERROR(error);
   }
   return pch;
}

std::vector<std::string> includesForLinkingTo(const std::string& linkingTo)
{
   std::vector<std::string> includes;
   r::exec::RFunction func(".rs.includesForLinkingTo", linkingTo);
   Error error = func.call(&includes);
   if (error)
   {
      error.addProperty("linking-to", linkingTo);
      LOG_ERROR(error);
   }
   return includes;
}




} // anonymous namespace


void RCompilationDatabase::updateForCurrentPackage()
{
   // check hash to see if we can avoid this computation
   std::string buildFileHash = packageBuildFileHash();
   if (buildFileHash == packageBuildFileHash_)
      return;

   // args to set
   std::vector<std::string> args;

   // start with clang compile args
   std::vector<std::string> clangArgs = clang().compileArgs(true);
   std::copy(clangArgs.begin(), clangArgs.end(), std::back_inserter(args));

   // read the package description file
   using namespace projects;
   FilePath pkgPath = projectContext().buildTargetPath();
   core::r_util::RPackageInfo pkgInfo;
   Error error = pkgInfo.read(pkgPath);
   if (error)
   {
      LOG_ERROR(error);
      return;
   }

   // Discover all of the LinkingTo relationships and add -I
   // arguments for them
   if (!pkgInfo.linkingTo().empty())
   {
      // Get includes implied by the LinkingTo field
      std::vector<std::string> includes = includesForLinkingTo(
                                                      pkgInfo.linkingTo());

      // add them to args
      std::copy(includes.begin(), includes.end(), std::back_inserter(args));
   }

   // get the build environment (e.g. Rtools config)
   core::system::Options env = compilationEnvironment();

   // Check for C++11 in SystemRequirements
   if (boost::algorithm::icontains(pkgInfo.systemRequirements(), "C++11"))
      env.push_back(std::make_pair("USE_CXX1X", "1"));

   // Run R CMD SHLIB
   FilePath srcDir = pkgPath.childPath("src");
   FilePath tempSrcFile = srcDir.childPath(
                                 core::system::generateUuid() + ".cpp");
   std::vector<std::string> compileArgs = argsForRCmdSHLIB(env, tempSrcFile);

   if (!compileArgs.empty())
   {
      // do path substitutions
      BOOST_FOREACH(std::string arg, compileArgs)
      {
         // do path substitutions
         boost::algorithm::replace_first(
                  arg,
                  "-I..",
                  "-I" + srcDir.parent().absolutePath());
         boost::algorithm::replace_first(
                  arg,
                  "-I.",
                  "-I" + srcDir.absolutePath());

         args.push_back(arg);
      }

      // set the args and build file hash (to avoid recomputation)
      packageSrcArgs_ = args;
      packagePCH_ = packagePCH(pkgInfo.linkingTo());
      packageBuildFileHash_ = buildFileHash;
   }

}

void RCompilationDatabase::updateForSourceCpp(const core::FilePath& srcFile)
{
   // read the the source cpp hash for this file
   std::string hash = sourceCppHash(srcFile);

   // check if we already have the args for this hash value
   std::string filename = srcFile.absolutePath();
   SourceCppHashes::const_iterator it = sourceCppHashes_.find(filename);
   if (it != sourceCppHashes_.end() && it->second == hash)
      return;

   // if there is no hash then bail (means this is not a sourceCpp file)
   if (hash.empty())
      return;

   // get args
   std::vector<std::string> args = argsForSourceCpp(srcFile);

   // save them
   if (!args.empty())
   {
      // update map
      sourceCppArgsMap_[srcFile.absolutePath()] = args;

      // save hash to prevent recomputation
      sourceCppHashes_[srcFile.absolutePath()] = hash;
   }
}


Error RCompilationDatabase::executeSourceCpp(
                                      core::system::Options env,
                                      const core::FilePath& srcPath,
                                      core::system::ProcessResult* pResult)
{
   // get path to R script
   FilePath rScriptPath;
   Error error = module_context::rScriptPath(&rScriptPath);
   if (error)
      return error;

   // establish options
   core::system::ProcessOptions options;

   // always run as a slave
   std::vector<std::string> args;
   args.push_back("--slave");

   // for packrat projects we execute the profile and set the working
   // directory to the project directory; for other contexts we just
   // propagate the R_LIBS
   if (module_context::packratContext().modeOn)
   {
      options.workingDir = projects::projectContext().directory();
      args.push_back("--no-save");
      args.push_back("--no-restore");
   }
   else
   {
      args.push_back("--vanilla");
      std::string libPaths = module_context::libPathsString();
      if (!libPaths.empty())
         core::system::setenv(&env, "R_LIBS", libPaths);
   }

   // we try to force --dry-run differently depending on the version of Rcpp
   std::string extraParams;
   if (module_context::isPackageVersionInstalled("Rcpp", "0.11.3"))
      extraParams = ", dryRun = TRUE";
   else
      core::system::setenv(&env, "MAKE", "make --dry-run");

   // set environment into options
   options.environment = env;

   // add command to arguments
   args.push_back("-e");
   boost::format fmt("Rcpp::sourceCpp('%1%', showOutput = TRUE%2%)");
   args.push_back(boost::str(fmt % srcPath.absolutePath() % extraParams));

   // execute and capture output
   return core::system::runProgram(
            core::string_utils::utf8ToSystem(rScriptPath.absolutePath()),
            args,
            "",
            options,
            pResult);
}

core::Error RCompilationDatabase::executeRCmdSHLIB(
                                 core::system::Options env,
                                 const core::FilePath& srcPath,
                                 core::system::ProcessResult* pResult)
{
   // get R bin directory
   FilePath rBinDir;
   Error error = module_context::rBinDir(&rBinDir);
   if (error)
      return error;

   // compile the file as dry-run
   module_context::RCommand rCmd(rBinDir);
   rCmd << "SHLIB";
   rCmd << "--dry-run";
   rCmd << srcPath.filename();

   // set options and run
   core::system::ProcessOptions options;
   options.workingDir = srcPath.parent();
   options.environment = env;
   return core::system::runCommand(rCmd.commandString(), options, pResult);
}


std::vector<std::string> RCompilationDatabase::compileArgsForTranslationUnit(
                                            const std::string& filename)
{
   // args to return
   std::vector<std::string> args;

   // get a file path object
   FilePath filePath(filename);

   // if this is a package source file then return the package args
   using namespace projects;
   std::string packagePCH;
   FilePath srcDirPath = projectContext().buildTargetPath().childPath("src");
   if ((projectContext().config().buildType == r_util::kBuildTypePackage) &&
       !filePath.relativePath(srcDirPath).empty())
   {
      // (re-)create on demand
      updateForCurrentPackage();

      // if we have args then capture them
      args = packageSrcArgs_;
      packagePCH = packagePCH_;
   }
   // otherwise lookup in the global dictionary
   else
   {
      // (re-)create on demand
      updateForSourceCpp(filePath);

      // if we have args then capture them
      std::string filename = filePath.absolutePath();
      ArgsMap::const_iterator it = sourceCppArgsMap_.find(filename);
      if (it != sourceCppArgsMap_.end())
      {
         args = it->second;
         packagePCH = "Rcpp";
      }
   }

   // bail if we have no args
   if (args.empty())
      return args;

   // add precompiled headers if necessary
   if (!packagePCH.empty())
   {
      std::string ext = filePath.extensionLowerCase();
      bool isCppFile = (ext == ".cc") || (ext == ".cpp");
      if (isCppFile)
      {
         // extract any -std= argument
         std::string stdArg = extractStdArg(args);

         std::vector<std::string> pchArgs = precompiledHeaderArgs(packagePCH,
                                                                  stdArg);
         std::copy(pchArgs.begin(),
                   pchArgs.end(),
                   std::back_inserter(args));
      }
   }

   // return args
   return args;
}

std::vector<std::string> RCompilationDatabase::translationUnits()
{
   using namespace projects;
   std::vector<FilePath> allSrcFiles;
   if (projectContext().config().buildType == r_util::kBuildTypePackage)
   {
      FilePath srcPath = projectContext().buildTargetPath().childPath("src");
      if (srcPath.exists())
      {
         Error error = srcPath.children(&allSrcFiles);
         if (!error)
         {
            std::vector<std::string> srcFiles;
            BOOST_FOREACH(const FilePath& srcFile, allSrcFiles)
            {
               std::string filename = srcFile.absolutePath();
               if (SourceIndex::isTranslationUnit(filename))
                  srcFiles.push_back(filename);
            }
            return srcFiles;
         }
         else
         {
            LOG_ERROR(error);
         }
      }
   }

   // no love
   return std::vector<std::string>();
}

std::vector<std::string> RCompilationDatabase::argsForSourceCpp(
                                                             FilePath srcFile)
{
   // build compile args
   std::vector<std::string> args;

   // start with clang compile args
   std::vector<std::string> clangCompileArgs = clang().compileArgs(true);
   std::copy(clangCompileArgs.begin(),
             clangCompileArgs.end(),
             std::back_inserter(args));

   // execute sourceCpp
   core::system::ProcessResult result;
   core::system::Options env = compilationEnvironment();
   Error error = executeSourceCpp(env, srcFile, &result);

   // process results of sourceCpp
   if (error)
   {
      LOG_ERROR(error);
      return std::vector<std::string>();
   }
   else if (result.exitStatus != EXIT_SUCCESS)
   {
      LOG_ERROR_MESSAGE("Error performing sourceCpp: " + result.stdErr);
      return std::vector<std::string>();
   }
   else
   {
      // parse the compilation results
      std::vector<std::string> compileArgs = parseCompilationResults(
                                                              srcFile,
                                                              result.stdOut);
      std::copy(compileArgs.begin(),
                compileArgs.end(),
                std::back_inserter(args));

      return args;
   }
}

std::vector<std::string> RCompilationDatabase::argsForRCmdSHLIB(
                                          core::system::Options env,
                                          FilePath tempSrcFile)
{
   Error error = core::writeStringToFile(tempSrcFile, "void foo() {}\n");
   if (error)
   {
      LOG_ERROR(error);
      return std::vector<std::string>();
   }

   // execute R CMD SHLIB
   core::system::ProcessResult result;
   error = executeRCmdSHLIB(env, tempSrcFile, &result);

   // remove the temporary source file
   Error removeError = tempSrcFile.remove();
   if (removeError)
      LOG_ERROR(removeError);

   // process results of R CMD SHLIB
   if (error)
   {
      LOG_ERROR(error);
      return std::vector<std::string>();
   }
   else if (result.exitStatus != EXIT_SUCCESS)
   {
      LOG_ERROR_MESSAGE("Error performing R CMD SHLIB: " + result.stdErr);
      return std::vector<std::string>();
   }
   else
   {
      // parse the compilation results
      return parseCompilationResults(tempSrcFile, result.stdOut);
   }
}


std::vector<std::string> RCompilationDatabase::rToolsArgs() const
{

#ifdef _WIN32
   if (rToolsArgs_.empty())
   {
      // scan for Rtools
      std::vector<core::r_util::RToolsInfo> rTools;
      Error error = core::r_util::scanRegistryForRTools(&rTools);
      if (error)
         LOG_ERROR(error);

      // enumerate them to see if we have a compatible version
      // (go in reverse order for most recent first)
      std::vector<r_util::RToolsInfo>::const_reverse_iterator it = rTools.rbegin();
      for ( ; it != rTools.rend(); ++it)
      {
         if (module_context::isRtoolsCompatible(*it))
         {
            FilePath rtoolsPath = it->installPath();

            rToolsArgs_.push_back("-I" + rtoolsPath.childPath(
               "gcc-4.6.3/i686-w64-mingw32/include").absolutePath());

            rToolsArgs_.push_back("-I" + rtoolsPath.childPath(
               "gcc-4.6.3/include/c++/4.6.3").absolutePath());

            std::string bits = "-I" + rtoolsPath.childPath(
               "gcc-4.6.3/include/c++/4.6.3/i686-w64-mingw32").absolutePath();
#ifdef _WIN64
            bits += "/64";
#endif
            rToolsArgs_.push_back(bits);

            break;
         }
      }
   }
#endif

   return rToolsArgs_;
}

std::vector<std::string> RCompilationDatabase::precompiledHeaderArgs(
                                                  const std::string& pkgName,
                                                  const std::string& stdArg)
{
   // args to return
   std::vector<std::string> args;

   // precompiled rcpp dir
   const std::string kPrecompiledDir = "libclang/precompiled/" + pkgName;
   FilePath precompiledDir = module_context::userScratchPath().
                                            childPath(kPrecompiledDir);

   // platform/rcpp version specific directory name
   std::string clangVersion = clang().version().asString();
   std::string platformDir;
   Error error = r::exec::RFunction(".rs.clangPCHPath", pkgName, clangVersion)
                                                         .call(&platformDir);
   if (error)
   {
      LOG_ERROR(error);
      return std::vector<std::string>();
   }

   // if this path doesn't exist then blow away all precompiled paths
   // and re-create this one. this enforces only storing precompiled headers
   // for the current version of R/Rcpp/pkg -- if we didn't do this then the
   // storage cost could really pile up over time (~25MB per PCH)
   FilePath platformPath = precompiledDir.childPath(platformDir);
   if (!platformPath.exists())
   {
      // delete root directory
      Error error = precompiledDir.removeIfExists();
      if (error)
      {
         LOG_ERROR(error);
         return std::vector<std::string>();
      }

      // create platform directory
      error = platformPath.ensureDirectory();
      if (error)
      {
         LOG_ERROR(error);
         return std::vector<std::string>();
      }
   }

   // now create the PCH if we need to
   FilePath pchPath = platformPath.childPath(pkgName + stdArg + ".pch");
   if (!pchPath.exists())
   {
      // state cpp file for creating precompiled headers
      FilePath cppPath = platformPath.childPath(pkgName + stdArg + ".cpp");
      std::string contents;
      boost::format fmt("#include <%1%.h>\n");
      contents.append(boost::str(fmt % pkgName));
      error = core::writeStringToFile(cppPath, contents);
      if (error)
      {
         LOG_ERROR(error);
         return std::vector<std::string>();
      }

      // compute args
      std::vector<std::string> args;

      // start with clang compile args
      std::vector<std::string> clangCompileArgs = clang().compileArgs(true);
      std::copy(clangCompileArgs.begin(),
                clangCompileArgs.end(),
                std::back_inserter(args));
      // -std argument
      if (!stdArg.empty())
         args.push_back(stdArg);

      // run R CMD SHLIB
      core::system::Options env = compilationEnvironment();
      FilePath tempSrcFile = module_context::tempFile("clang", "cpp");
      std::vector<std::string> cArgs = argsForRCmdSHLIB(env, tempSrcFile);
      std::copy(cArgs.begin(), cArgs.end(), std::back_inserter(args));

      // add this package's path to the args
      std::vector<std::string> pkgArgs = includesForLinkingTo(pkgName);
      std::copy(pkgArgs.begin(), pkgArgs.end(), std::back_inserter(args));

      // create args array
      core::system::ProcessArgs argsArray(args);

      CXIndex index = clang().createIndex(0,0);

      CXTranslationUnit tu = clang().parseTranslationUnit(
                            index,
                            cppPath.absolutePath().c_str(),
                            argsArray.args(),
                            argsArray.argCount(),
                            0,
                            0,
                            CXTranslationUnit_ForSerialization);
      if (tu == NULL)
      {
         LOG_ERROR_MESSAGE("Error parsing translation unit " +
                           cppPath.absolutePath());
         clang().disposeIndex(index);
         return std::vector<std::string>();
      }

      int ret = clang().saveTranslationUnit(tu,
                                            pchPath.absolutePath().c_str(),
                                            clang().defaultSaveOptions(tu));
      if (ret != CXSaveError_None)
      {
         boost::format fmt("Error %1% saving translation unit %2%");
         std::string msg = boost::str(fmt % ret % pchPath.absolutePath());
         LOG_ERROR_MESSAGE(msg);
      }

      clang().disposeTranslationUnit(tu);

      clang().disposeIndex(index);
   }

   // reutrn the pch header file args
   args.push_back("-include-pch");
   args.push_back(pchPath.absolutePath());
   return args;
}

} // namespace clang
} // namespace modules
} // namesapce session

