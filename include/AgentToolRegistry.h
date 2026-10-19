/*
 * AgentToolRegistry.h - Registry for AI Agent tools in LMMS
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

#ifndef LMMS_AGENT_TOOL_REGISTRY_H
#define LMMS_AGENT_TOOL_REGISTRY_H

#include <functional>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QMap>
#include <QList>

namespace lmms
{

using ToolHandler = std::function<QJsonObject(const QJsonObject& args, bool& ok, QString& errorMsg)>;

struct AgentTool
{
	QString name;
	QString description;
	QJsonObject inputSchema;
	ToolHandler handler;
};

class AgentToolRegistry
{
public:
	static AgentToolRegistry& instance();

	void registerTool(const QString& name,
	                  const QString& description,
	                  const QJsonObject& properties,
	                  const QJsonArray& required,
	                  ToolHandler handler);

	void registerTool(const AgentTool& tool);

	QJsonArray manifest() const;

	QJsonObject execute(const QString& name,
	                    const QJsonObject& args,
	                    bool& ok,
	                    QString& errorMsg) const;

	bool hasTool(const QString& name) const;
	int toolCount() const { return m_tools.size(); }

	void clear();

private:
	AgentToolRegistry() = default;
	QMap<QString, AgentTool> m_tools;
	QList<QString> m_toolOrder;
};

} // namespace lmms

#endif // LMMS_AGENT_TOOL_REGISTRY_H
