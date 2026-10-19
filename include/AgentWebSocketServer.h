/*
 * AgentWebSocketServer.h - WebSocket MCP server for AI Agent control in LMMS
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

#ifndef LMMS_AGENT_WEBSOCKET_SERVER_H
#define LMMS_AGENT_WEBSOCKET_SERVER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QList>

class QWebSocketServer;
class QWebSocket;

namespace lmms
{

class AgentWebSocketServer : public QObject
{
	Q_OBJECT
public:
	static AgentWebSocketServer* instance();

	void start(quint16 port = 8765);
	void stop();

	bool isListening() const;
	quint16 port() const { return m_port; }

	// Returns the MCP-compatible list of tools
	QJsonArray toolsManifest() const;

	// Constructs the welcome/handshake payload sent to clients on connect
	QJsonObject greetingPayload() const;

public slots:
	void broadcast(const QJsonObject& message);

private slots:
	void onNewConnection();
	void onSocketDisconnected();
	void onTextMessageReceived(const QString& message);

private:
	explicit AgentWebSocketServer(QObject* parent = nullptr);
	~AgentWebSocketServer() override;

	void initTools();
	void handleRequest(const QJsonObject& req, QWebSocket* client);
	void sendJson(QWebSocket* client, const QJsonObject& obj);

	// Tool execution dispatcher
	QJsonObject executeTool(const QString& name, const QJsonObject& arguments, bool& ok, QString& errorMsg);

	static AgentWebSocketServer* s_instance;

	QWebSocketServer* m_server;
	QList<QWebSocket*> m_clients;
	quint16 m_port;
};

} // namespace lmms

#endif // LMMS_AGENT_WEBSOCKET_SERVER_H
