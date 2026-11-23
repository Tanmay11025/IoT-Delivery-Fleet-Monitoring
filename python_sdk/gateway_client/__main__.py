"""Small command-line wrapper for manual testing.

    python -m gateway_client publish '{"vehicle_id":"van-1", ...}'
    python -m gateway_client tail [--from-offset N]
    python -m gateway_client vehicles
"""

from __future__ import annotations

import argparse
import json
import sys

from .client import GatewayClient, GatewayError, Message


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(prog="python -m gateway_client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--http-port", type=int, default=8080)
    parser.add_argument("--consumer-port", type=int, default=9090)
    commands = parser.add_subparsers(dest="command", required=True)
    publish = commands.add_parser("publish", help="publish one telemetry event (JSON)")
    publish.add_argument("event")
    tail = commands.add_parser("tail", help="print records as they arrive")
    tail.add_argument("--from-offset", type=int, default=None)
    commands.add_parser("vehicles", help="print the latest state of every vehicle")
    args = parser.parse_args(argv)

    client = GatewayClient(args.host, args.http_port, args.consumer_port)
    try:
        if args.command == "publish":
            print(client.send_telemetry(json.loads(args.event)))
        elif args.command == "vehicles":
            print(json.dumps(client.vehicles(), indent=2))
        else:
            def show(message: Message) -> None:
                print(f"{message.offset:>10}  {message.payload.decode('utf-8', 'replace')}",
                      flush=True)
            client.subscribe_stream(show, from_offset=args.from_offset)
    except KeyboardInterrupt:
        client.disconnect()
    except (GatewayError, ValueError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
