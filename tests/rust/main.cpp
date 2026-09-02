// Trivial entry point for the su_core_rust_tests target. The binary itself is
// never executed; the target exists only so xmake's on_test runs `cargo test`
// for the Rust core. A real build artifact is required for on_test to report
// pass/fail reliably.
int main() {
    return 0;
}
