import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext) {
    const config = vscode.workspace.getConfiguration('mirage');
    const command = config.get<string>('lspPath', 'mirage-lsp');
    const libraryPath = config.get<string>('libraryPath', '');

    const serverOptions: ServerOptions = {
        command,
        args: [],
        transport: TransportKind.stdio,
        // Passing a partial `env` to child_process.spawn replaces the whole
        // environment rather than merging it, so process.env is spread first -
        // otherwise setting MIRAGE_PATH here would drop everything else
        // (PATH included) from the spawned server's environment.
        ...(libraryPath ? { options: { env: { ...process.env, MIRAGE_PATH: libraryPath } } } : {}),
    };

    const clientOptions: LanguageClientOptions = {
        documentSelector: [{ scheme: 'file', language: 'mirage' }],
    };

    client = new LanguageClient('mirageLsp', 'Mirage Language Server', serverOptions, clientOptions);
    context.subscriptions.push(client);
    void client.start();
}

export function deactivate(): Thenable<void> | undefined {
    return client ? client.stop() : undefined;
}
