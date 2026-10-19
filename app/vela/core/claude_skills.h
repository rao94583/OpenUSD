// The Claude Code plugin that gives the Claude panel Vela's skills
// (usd-python, ui, plugins, image-generation). The bundle holds its fixed
// part, with the schema reference generated at build time; each launch of
// the CLI copies it to a folder of its own and adds what only the running
// app knows: the actions reference, which includes plugins' commands.
#pragma once

#include "actions.h"

#include <string>

namespace vela::claude {

// Every action as Markdown, grouped by menu: name, title, parameters and
// whether it only queries, edits the scene or asks the user.
std::string ActionsReference(const ActionRegistry &registry);

struct SkillOptions {
    // The image-generation skill is left out while the feature is off.
    bool imageGeneration = false;
    // Replaces PLUGINS_FOLDER in the plugins skill.
    std::string pluginsFolder;
};

// Copies the bundle's plugin (source) to destination, replacing what is
// there, and writes skills/ui/references/actions.md. Returns false with a
// message on failure.
bool InstallSkills(const std::string &source, const std::string &destination,
                   const ActionRegistry &registry, const SkillOptions &options,
                   std::string *error);

// Where InstallSkills puts the plugin: next to the conversation history.
std::string SkillsFolder();

} // namespace vela::claude
