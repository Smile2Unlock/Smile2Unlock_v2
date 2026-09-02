import std;
import su.auth.daemon;
import su.control.socket;

int main(int argc, char** argv) {
    const auto socket_path = argc == 2
        ? std::string_view(argv[1])
        : su::control::kDefaultSocketPath;
    return su::auth::run_daemon(socket_path);
}
