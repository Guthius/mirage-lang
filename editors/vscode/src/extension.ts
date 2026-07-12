import * as vscode from 'vscode';
import {
    LanguageClient,
    LanguageClientOptions,
    ServerOptions,
    TransportKind,
} from 'vscode-languageclient/node';

let client: LanguageClient | undefined;

export function activate(context: vscode.ExtensionContext) {
    const command = vscode.workspace.getConfiguration('mirage').get<string>('lspPath', 'mirage-lsp');

    const serverOptions: ServerOptions = {
        command,
        args: [],
        transport: TransportKind.stdio,
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
