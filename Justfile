build_dir := "build"
cxx_compiler := "clang++"
cxx_standard := "23"
vscode_dir := "editors/vscode"

# Clean build artifacts
clean:
    rm -f compile_commands.json
    rm -rf {{ build_dir }}

# Configure the project
configure:
    cmake -S . \
        -B {{ build_dir }} \
        -D CMAKE_CXX_COMPILER="{{ cxx_compiler }}" \
        -D CMAKE_CXX_STANDARD={{ cxx_standard }} \
        -D CMAKE_CXX_STANDARD_REQUIRED=ON \
        -D CMAKE_EXPORT_COMPILE_COMMANDS=ON \
        -D CMAKE_BUILD_TYPE=Debug \
        -G Ninja

    ln -sf {{ build_dir }}/compile_commands.json .

# Build the server
build target="all": configure
    cmake --build {{ build_dir }} --target {{ target }}

# Run the compiler
run: build
    ./{{ build_dir }}/mirage run examples/start

# Install the compiler to the system
install prefix="/usr/local": build
    install -Dm755 {{ build_dir }}/mirage {{ prefix }}/bin/mirage

# Install mirage-lsp to the system
install-lsp prefix="/usr/local": build
    install -Dm755 {{ build_dir }}/mirage-lsp {{ prefix }}/bin/mirage-lsp

# Install all binaries
install-all: install install-lsp

# Package the VS Code extension into a .vsix installable via `code --install-extension`
package-vscode:
    cd {{ vscode_dir }} && npm install && npm run package

# Format all source files that are part of the project
format:
    find ./src -type f \( -name '*.cpp' \) \
        -print0 | xargs -0 clang-format -i
    find ./src -type f \( -name '*.hpp' \) \
        -print0 | xargs -0 clang-format -i

# Run the full test battery (ctest + every tests/*_test.py)
test: build
    #!/usr/bin/env bash
    # Two suites need the standard library for 'core/testing'. MIRAGE_STD defaults to a
    # sibling checkout of the Mirage repo; override it if yours lives elsewhere:
    #
    #     just test
    #     MIRAGE_STD=/path/to/Mirage just test
    #
    # Every suite runs even after one fails, and the full list is reported at the end — one
    # early failure otherwise hides whatever else broke.
    set -uo pipefail
    export MIRAGE_STD="${MIRAGE_STD:-$(cd .. && pwd)/Mirage}"
    if [ ! -d "$MIRAGE_STD/core/testing" ]; then
      echo "error: no stdlib at $MIRAGE_STD (needs core/testing)" >&2
      echo "       set MIRAGE_STD to a checkout of the Mirage standard library" >&2
      exit 1
    fi
    failed=()
    ( cd {{ build_dir }} && ctest --output-on-failure ) || failed+=("ctest")
    for t in tests/*_test.py; do
      echo "=== $t ==="
      python3 "$t" || failed+=("$t")
    done
    echo
    if [ ${#failed[@]} -ne 0 ]; then
      printf 'FAILED: %s\n' "${failed[@]}" >&2
      exit 1
    fi
    echo "all green"
