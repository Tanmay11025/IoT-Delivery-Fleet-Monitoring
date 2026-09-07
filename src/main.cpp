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
    
    // The optional arguments are: port and connection backlog
    if (argc > 3) {
        cerr << "Usage: " << argv[0] << " [port] [backlog]\n";
        return EXIT_FAILURE;
    }

    int port = 8080;
    int backlog = 128;
    if ((argc > 1 && !parse_argument(argv[1], "port", port)) ||
        (argc > 2 && !parse_argument(argv[2], "backlog", backlog))) {
        return EXIT_FAILURE;
    }

    if (port < 1 || port > 65535 || backlog < 1) {
        cerr << "Error: port must be between 1 and 65535, and backlog must be positive\n";
        return EXIT_FAILURE;
    }

    // The server object owns the socket and cleans it up automatically
    TCPServer server(port, backlog);

    // start() returns false for operating-system setup failures
    if (!server.start()) {
        return EXIT_FAILURE;
    }

    return 0;
}
