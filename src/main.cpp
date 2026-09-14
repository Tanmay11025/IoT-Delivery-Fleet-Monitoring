#include "network/tcp_server.h"

// Convert one command-line value to an integer and reject invalid text.
bool parse_argument(const char* value, const char* name, int& result) {
    stringstream input(value);
    char extra_character;

    if (!(input >> result) || input >> extra_character) {
        cerr << "Invalid " << name << ": expected an integer\n";
        return false;
    }

    return true;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, handle_sigint);
    
    // The optional arguments are: port, connection backlog, and worker count.
    if (argc > 4) {
        cerr << "Usage: " << argv[0] << " [port] [backlog] [workers]\n";
        return EXIT_FAILURE;
    }

    int port = 8080;
    int backlog = 128;
    int workers = 0;
    if ((argc > 1 && !parse_argument(argv[1], "port", port)) ||
        (argc > 2 && !parse_argument(argv[2], "backlog", backlog)) ||
        (argc > 3 && !parse_argument(argv[3], "workers", workers))) {
        return EXIT_FAILURE;
    }

    if (port < 1 || port > 65535 || backlog < 1 || workers < 0) {
        cerr << "Error: port must be between 1 and 65535, backlog must be positive, "
             << "and workers must not be negative\n";
        return EXIT_FAILURE;
    }

    // The server object owns the socket and cleans it up automatically
    TCPServer server(port, backlog, static_cast<unsigned int>(workers));

    // start() returns false for operating-system setup failures
    if (!server.start()) {
        return EXIT_FAILURE;
    }

    return 0;
}
