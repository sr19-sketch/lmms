/*
 * AgentProjectTools.h - Project management tools for AI Agent in LMMS
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

#ifndef LMMS_AGENT_PROJECT_TOOLS_H
#define LMMS_AGENT_PROJECT_TOOLS_H

namespace lmms
{

class AgentToolRegistry;

void registerProjectTools(AgentToolRegistry& registry);

} // namespace lmms

#endif // LMMS_AGENT_PROJECT_TOOLS_H
