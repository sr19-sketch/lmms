/*
 * AgentWebSocketServer.cpp - WebSocket MCP server for AI Agent control in LMMS
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

#include "AgentWebSocketServer.h"
#include "AgentToolRegistry.h"
#include "AgentProjectTools.h"
#include "AgentTrackTools.h"
#include "AgentInstrumentTools.h"
#include "AgentMixerTools.h"
#include "AgentVstTools.h"

#include <QtWebSockets/QWebSocketServer>
#include <QtWebSockets/QWebSocket>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

namespace lmms
{

AgentWebSocketServer* AgentWebSocketServer::s_instance = nullptr;

AgentWebSocketServer* AgentWebSocketServer::instance()
{
	if (!s_instance)
	{
		s_instance = new AgentWebSocketServer(QCoreApplication::instance());
	}
	return s_instance;
}

AgentWebSocketServer::AgentWebSocketServer(QObject* parent) :
	QObject(parent),
	m_server(nullptr),
	m_port(8765)
{
	m_server = new QWebSocketServer(
		QStringLiteral("LMMS Agent Server"),
		QWebSocketServer::NonSecureMode,
		this
	);

	connect(m_server, &QWebSocketServer::newConnection,
	        this, &AgentWebSocketServer::onNewConnection);

	initTools();
}

AgentWebSocketServer::~AgentWebSocketServer()
{
	stop();
}

void AgentWebSocketServer::initTools()
{
	auto& reg = AgentToolRegistry::instance();
	reg.clear();

	registerProjectTools(reg);
	registerTrackTools(reg);
	registerInstrumentTools(reg);
	registerMixerTools(reg);
	registerVstTools(reg);

	qDebug() << "[AgentServer] Registered" << reg.toolCount() << "agent tools.";
}

void AgentWebSocketServer::start(quint16 port)
{
	m_port = port;

	if (m_server->isListening())
	{
		m_server->close();
	}

	if (m_server->listen(QHostAddress::Any, m_port))
	{
		qDebug() << "[AgentServer] WebSocket MCP server started and listening on port" << m_port;
	}
	else
	{
		qWarning() << "[AgentServer] Failed to start WebSocket server on port"
		           << m_port << ":" << m_server->errorString();
	}
}

void AgentWebSocketServer::stop()
{
	if (m_server && m_server->isListening())
	{
		for (QWebSocket* client : m_clients)
		{
			if (client)
			{
				client->close(QWebSocketProtocol::CloseCodeNormal, QStringLiteral("Server stopping"));
			}
		}
		qDeleteAll(m_clients);
		m_clients.clear();
		m_server->close();
		qDebug() << "[AgentServer] WebSocket MCP server stopped.";
	}
}

bool AgentWebSocketServer::isListening() const
{
	return m_server && m_server->isListening();
}

QJsonArray AgentWebSocketServer::toolsManifest() const
{
	return AgentToolRegistry::instance().manifest();
}

QJsonObject AgentWebSocketServer::greetingPayload() const
{
	QJsonObject greeting;
	greeting[QStringLiteral("type")] = QStringLiteral("connection_established");
	greeting[QStringLiteral("server")] = QStringLiteral("LMMS Agent WebSocket Server");
	greeting[QStringLiteral("version")] = QStringLiteral("1.0");
	greeting[QStringLiteral("port")] = m_port;
	greeting[QStringLiteral("message")] = QStringLiteral("Connected to LMMS. Available tools are listed in 'tools'.");
	greeting[QStringLiteral("tools")] = toolsManifest();
	return greeting;
}

void AgentWebSocketServer::broadcast(const QJsonObject& message)
{
	QJsonDocument doc(message);
	QString text = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));

	for (QWebSocket* client : m_clients)
	{
		if (client && client->isValid())
		{
			client->sendTextMessage(text);
		}
	}
}

void AgentWebSocketServer::onNewConnection()
{
	QWebSocket* client = m_server->nextPendingConnection();
	if (!client)
	{
		return;
	}

	connect(client, &QWebSocket::textMessageReceived,
	        this, &AgentWebSocketServer::onTextMessageReceived);
	connect(client, &QWebSocket::disconnected,
	        this, &AgentWebSocketServer::onSocketDisconnected);

	m_clients.append(client);

	qDebug() << "[AgentServer] Client connected from"
	         << client->peerAddress().toString()
	         << "Total clients:" << m_clients.size();

	sendJson(client, greetingPayload());
}

void AgentWebSocketServer::onSocketDisconnected()
{
	auto* client = qobject_cast<QWebSocket*>(sender());
	if (client)
	{
		m_clients.removeAll(client);
		client->deleteLater();
		qDebug() << "[AgentServer] Client disconnected. Remaining clients:" << m_clients.size();
	}
}

void AgentWebSocketServer::onTextMessageReceived(const QString& message)
{
	auto* client = qobject_cast<QWebSocket*>(sender());
	if (!client)
	{
		return;
	}

	QJsonParseError parseError;
	QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);

	if (parseError.error != QJsonParseError::NoError || !doc.isObject())
	{
		QJsonObject errResp;
		errResp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
		errResp[QStringLiteral("id")] = QJsonValue::Null;

		QJsonObject errorObj;
		errorObj[QStringLiteral("code")] = -32700;
		errorObj[QStringLiteral("message")] = QStringLiteral("Parse error: ") + parseError.errorString();
		errResp[QStringLiteral("error")] = errorObj;

		sendJson(client, errResp);
		return;
	}

	handleRequest(doc.object(), client);
}

void AgentWebSocketServer::sendJson(QWebSocket* client, const QJsonObject& obj)
{
	if (!client || !client->isValid())
	{
		return;
	}
	QJsonDocument doc(obj);
	client->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void AgentWebSocketServer::handleRequest(const QJsonObject& req, QWebSocket* client)
{
	QJsonValue idVal = req.value(QStringLiteral("id"));
	QString method = req.value(QStringLiteral("method")).toString();
	QJsonObject params = req.value(QStringLiteral("params")).toObject();

	// Support both JSON-RPC method style and plain message type style
	if (method.isEmpty() && req.contains(QStringLiteral("type")))
	{
		method = req.value(QStringLiteral("type")).toString();
	}

	// 1. tools/list or get_tools
	if (method == QStringLiteral("tools/list") || method == QStringLiteral("get_tools"))
	{
		QJsonObject resp;
		resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
		resp[QStringLiteral("id")] = idVal;

		QJsonObject result;
		result[QStringLiteral("tools")] = toolsManifest();
		resp[QStringLiteral("result")] = result;
		resp[QStringLiteral("tools")] = toolsManifest();

		sendJson(client, resp);
		return;
	}

	// 2. ping
	if (method == QStringLiteral("ping"))
	{
		QJsonObject resp;
		resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
		resp[QStringLiteral("id")] = idVal;
		QJsonObject result;
		result[QStringLiteral("pong")] = true;
		resp[QStringLiteral("result")] = result;
		sendJson(client, resp);
		return;
	}

	// 3. tools/call or call_tool or execute_tool or tool_call
	if (method == QStringLiteral("tools/call") ||
	    method == QStringLiteral("call_tool") ||
	    method == QStringLiteral("execute_tool") ||
	    method == QStringLiteral("tool_call"))
	{
		QString toolName = params.value(QStringLiteral("name")).toString();
		QJsonObject toolArgs = params.value(QStringLiteral("arguments")).toObject();

		if (toolName.isEmpty() && req.contains(QStringLiteral("tool")))
		{
			toolName = req.value(QStringLiteral("tool")).toString();
			toolArgs = req.value(QStringLiteral("arguments")).toObject();
		}
		if (toolName.isEmpty() && req.contains(QStringLiteral("name")))
		{
			toolName = req.value(QStringLiteral("name")).toString();
		}
		if (toolArgs.isEmpty() && req.contains(QStringLiteral("args")))
		{
			toolArgs = req.value(QStringLiteral("args")).toObject();
		}

		bool ok = false;
		QString errorMsg;
		QJsonObject toolResult = executeTool(toolName, toolArgs, ok, errorMsg);

		QJsonObject resp;
		resp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
		resp[QStringLiteral("type")] = QStringLiteral("tool_result");
		resp[QStringLiteral("id")] = idVal;
		resp[QStringLiteral("success")] = ok;

		if (!ok)
		{
			resp[QStringLiteral("status")] = QStringLiteral("error");
			resp[QStringLiteral("error")] = errorMsg;
			resp[QStringLiteral("isError")] = true;

			QJsonObject mcpResult;
			QJsonArray contentArray;
			QJsonObject textObj;
			textObj[QStringLiteral("type")] = QStringLiteral("text");
			textObj[QStringLiteral("text")] = errorMsg;
			contentArray.append(textObj);
			mcpResult[QStringLiteral("content")] = contentArray;
			mcpResult[QStringLiteral("isError")] = true;
			resp[QStringLiteral("result")] = mcpResult;
		}
		else
		{
			resp[QStringLiteral("status")] = QStringLiteral("success");
			resp[QStringLiteral("data")] = toolResult;
			resp[QStringLiteral("message")] = toolResult.value(QStringLiteral("message")).toString(
				QStringLiteral("Tool '%1' executed successfully.").arg(toolName)
			);

			QJsonObject mcpResult;
			QJsonArray contentArray;
			QJsonObject textObj;
			textObj[QStringLiteral("type")] = QStringLiteral("text");
			textObj[QStringLiteral("text")] = toolResult.value(QStringLiteral("message")).toString(
				QStringLiteral("Tool '%1' executed successfully.").arg(toolName)
			);
			contentArray.append(textObj);
			mcpResult[QStringLiteral("content")] = contentArray;
			mcpResult[QStringLiteral("data")] = toolResult;
			mcpResult[QStringLiteral("isError")] = false;
			resp[QStringLiteral("result")] = mcpResult;
		}

		sendJson(client, resp);
		return;
	}

	// 4. Unknown method
	QJsonObject errResp;
	errResp[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
	errResp[QStringLiteral("id")] = idVal;
	QJsonObject errObj;
	errObj[QStringLiteral("code")] = -32601;
	errObj[QStringLiteral("message")] = QStringLiteral("Method not found: ") + method;
	errResp[QStringLiteral("error")] = errObj;
	sendJson(client, errResp);
}

QJsonObject AgentWebSocketServer::executeTool(const QString& name, const QJsonObject& arguments, bool& ok, QString& errorMsg)
{
	return AgentToolRegistry::instance().execute(name, arguments, ok, errorMsg);
}

} // namespace lmms
