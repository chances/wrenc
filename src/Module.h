//
// Created by znix on 10/07/2022.
//

#pragma once

#include "IRNode.h"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

class Module {
  public:
	const std::optional<std::string> &Name() const { return m_name; }

	IRGlobalDecl *AddVariable(const std::string &name);

	IRGlobalDecl *FindVariable(const std::string &name);

	std::vector<IRGlobalDecl *> GetGlobalVariables();

	/// Add a top-level node.
	void AddNode(IRNode *node);

  private:
	std::optional<std::string> m_name;
	std::unordered_map<std::string, std::unique_ptr<IRGlobalDecl>> m_globals;
};
