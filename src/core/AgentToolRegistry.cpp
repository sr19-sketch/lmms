/*
 * AgentToolRegistry.cpp - Registry for AI Agent tools in LMMS
 *
 * Copyright (c) 2026 LMMS Developers
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 */

#include "AgentToolRegistry.h"

namespace lmms
{

AgentToolRegistry& AgentToolRegistry::instance()
{
	static AgentToolRegistry s_instance;
	return s_instance;
}

void AgentToolRegistry::registerTool(const QString& name,
                                     const QString& description,
                                     const QJsonObject& properties,
                                     const QJsonArray& required,
                                     ToolHandler handler)
{
	QJsonObject inputSchema;
	inputSchema[QStringLiteral("type")] = QStringLiteral("object");
	inputSchema[QStringLiteral("properties")] = properties;
	if (!required.isEmpty())
	{
		inputSchema[QStringLiteral("required")] = required;
	}

	AgentTool tool;
	tool.name = name;
	tool.description = description;
	tool.inputSchema = inputSchema;
	tool.handler = std::move(handler);

	registerTool(tool);
}

void AgentToolRegistry::registerTool(const AgentTool& tool)
{
	if (!m_tools.contains(tool.name))
	{
		m_toolOrder.append(tool.name);
	}
	m_tools.insert(tool.name, tool);
}

QJsonArray AgentToolRegistry::manifest() const
{
	QJsonArray array;
	for (const QString& name : m_toolOrder)
	{
		if (!m_tools.contains(name)) continue;
		const AgentTool& tool = m_tools.value(name);

		QJsonObject toolObj;
		toolObj[QStringLiteral("name")] = tool.name;
		toolObj[QStringLiteral("description")] = tool.description;
		toolObj[QStringLiteral("inputSchema")] = tool.inputSchema;
		array.append(toolObj);
	}
	return array;
}

QJsonObject AgentToolRegistry::execute(const QString& name,
                                       const QJsonObject& args,
                                       bool& ok,
                                       QString& errorMsg) const
{
	if (!m_tools.contains(name))
	{
		ok = false;
		errorMsg = QStringLiteral("Unknown tool: '%1'").arg(name);
		return QJsonObject();
	}

	const AgentTool& tool = m_tools.value(name);
	if (!tool.handler)
	{
		ok = false;
		errorMsg = QStringLiteral("Tool '%1' does not have an execution handler.").arg(name);
		return QJsonObject();
	}

	return tool.handler(args, ok, errorMsg);
}

bool AgentToolRegistry::hasTool(const QString& name) const
{
	return m_tools.contains(name);
}

void AgentToolRegistry::clear()
{
	m_tools.clear();
	m_toolOrder.clear();
}

} // namespace lmms
