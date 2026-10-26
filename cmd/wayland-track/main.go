package main

import (
	"os"

	"wayland-track/internal/cli"
)

func main() {
	os.Exit(cli.Main(os.Args[1:]))
}
