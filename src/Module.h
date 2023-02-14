//
// Created by znix on 10/07/2022.
//

#pragma once

#include "IRNode.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class ClassInfo;

class Module {
  public:
	DLL_EXPORT Module(std::string name);
	DLL_EXPORT ~Module();

	DLL_EXPORT const std::string &Name() const { return m_name; }

	DLL_EXPORT IRGlobalDecl *AddVariable(const std::string &name);

	DLL_EXPORT IRGlobalDecl *FindVariable(const std::string &name);

	DLL_EXPORT std::vector<IRGlobalDecl *> GetGlobalVariables();

	/// Get all the functions in the module. The main function (that runs when the module is imported) is the
	/// first function in this list.
	DLL_EXPORT const std::vector<IRFn *> &GetFunctions() const;

	DLL_EXPORT std::vector<IRFn *> GetClosures() const;

	/// Get the module's initialisation function. Same as GetFunctions.at(0);
	DLL_EXPORT const IRFn *GetMainFunction() const;

	DLL_EXPORT const std::vector<IRClass *> &GetClasses() const;

	/// Add a top-level node.
	DLL_EXPORT void AddNode(IRNode *node);

	std::optional<std::string> sourceFilePath;

  private:
	std::string m_name;
	std::unordered_map<std::string, std::unique_ptr<IRGlobalDecl>> m_globals;
	std::vector<IRClass *> m_classes;
	std::vector<IRFn *> m_functions;
};
