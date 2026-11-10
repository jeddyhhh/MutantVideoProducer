#pragma once
#include <QStringList>
class McpServer;
class Project;
// AI control with hostile arguments: `rounds` calls of random tools (those that don't write files, render, download or
// use devices) with random, extreme and existing-id arguments made from each tool's schema. After each call the
// answer must be a result or an error, the timeline sound, and no message box open. MVP_MCP_FUZZ_SEED picks another
// sequence.
int runMcpFuzz(McpServer* server, Project* project, QStringList& log, int rounds);
