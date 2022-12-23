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
	Module();
	Module(std::optional<std::string> name);
	~Module();

	const std::optional<std::string> &Name() const { return m_name; }

	IRGlobalDecl *AddVariable(const std::string &name);

	IRGlobalDecl *FindVariable(const std::string &name);

	std::vector<IRGlobalDecl *> GetGlobalVariables();

	/// Get all the functions in the module. The main function (that runs when the module is imported) is the
	/// first function in this list.
	const std::vector<IRFn *> &GetFunctions() const;

	std::vector<IRFn *> GetClosures() const;

	/// Get the module's initialisation function. Same as GetFunctions.at(0);
	const IRFn *GetMainFunction() const;

	const std::vector<IRClass *> &GetClasses() const;

	/// Add a top-level node.
	void AddNode(IRNode *node);

	std::optional<std::string> sourceFilePath;

  private:
	std::optional<std::string> m_name;
	std::unordered_map<std::string, std::unique_ptr<IRGlobalDecl>> m_globals;
	std::vector<IRClass *> m_classes;
	std::vector<IRFn *> m_functions;
};
