use std::collections::HashMap;

use zed_extension_api::{self as zed, LanguageServerId, Result, settings::LspSettings};

const SERVER_ID: &str = "mirage-lsp";

struct MirageExtension;

impl zed::Extension for MirageExtension {
    fn new() -> Self {
        MirageExtension
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &LanguageServerId,
        worktree: &zed::Worktree,
    ) -> Result<zed::Command> {
        let binary = LspSettings::for_worktree(SERVER_ID, worktree)
            .unwrap_or_default()
            .binary;

        let path = binary
            .as_ref()
            .and_then(|binary| binary.path.clone())
            .or_else(|| worktree.which(SERVER_ID))
            .ok_or_else(|| {
                "mirage-lsp not found on PATH. Install it, or set `lsp.mirage-lsp.binary.path` \
                 in your Zed settings.json."
                    .to_string()
            })?;

        let args = binary
            .as_ref()
            .and_then(|binary| binary.arguments.clone())
            .unwrap_or_default();

        // Start from the worktree's shell environment (so PATH etc. resolve the
        // same way a terminal-launched `mirage-lsp` would), then layer any
        // `lsp.mirage-lsp.binary.env` setting on top. That's how a user sets
        // MIRAGE_PATH for standard-library import resolution when Zed itself
        // wasn't launched from a shell that already had it exported.
        let mut env: HashMap<String, String> = worktree.shell_env().into_iter().collect();
        if let Some(extra_env) = binary.and_then(|binary| binary.env) {
            env.extend(extra_env);
        }

        Ok(zed::Command {
            command: path,
            args,
            env: env.into_iter().collect(),
        })
    }
}

zed::register_extension!(MirageExtension);
