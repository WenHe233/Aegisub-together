package main

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"syscall"
	"time"

	collab "github.com/WenHe233/Aegisub-together/server"
	"github.com/WenHe233/Aegisub-together/server/internal/auth"
)

func main() {
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, os.Args[1:], os.Stdin, os.Stdout, os.Stderr); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(ctx context.Context, args []string, stdin io.Reader, stdout, stderr io.Writer) error {
	if len(args) == 0 {
		return usageError()
	}
	switch args[0] {
	case "serve":
		return runServe(ctx, args[1:], stdout, stderr)
	case "hash-password":
		return runHashPassword(args[1:], stdin, stdout, stderr)
	case "backup":
		return runBackup(ctx, args[1:], stderr)
	case "rooms":
		return runRooms(ctx, args[1:], stdout, stderr)
	default:
		return usageError()
	}
}

func runServe(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	flags := flag.NewFlagSet("serve", flag.ContinueOnError)
	flags.SetOutput(stderr)
	listen := flags.String("listen", envOr("AEGISUB_COLLAB_LISTEN", ":8080"), "HTTP listen address")
	database := flags.String("database", envOr("AEGISUB_COLLAB_DATABASE", "collab.db"), "SQLite database path")
	accessHash := flags.String("access-password-hash", os.Getenv("AEGISUB_COLLAB_ACCESS_PASSWORD_HASH"), "Argon2id access-password hash")
	archiveDays := flags.Int("archive-days", 0, "archive inactive rooms after this many days; 0 disables")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 || *archiveDays < 0 {
		return errors.New("serve accepts flags only and archive-days cannot be negative")
	}
	server, err := collab.New(collab.Config{
		AccessPasswordHash: *accessHash, DatabasePath: *database, ArchiveAfter: time.Duration(*archiveDays) * 24 * time.Hour,
	})
	if err != nil {
		return err
	}
	defer server.Close()
	httpServer := &http.Server{Addr: *listen, Handler: server, ReadHeaderTimeout: 10 * time.Second}
	errorsChannel := make(chan error, 1)
	go func() { errorsChannel <- httpServer.ListenAndServe() }()
	fmt.Fprintf(stdout, "listening on %s\n", *listen)
	select {
	case <-ctx.Done():
		shutdownContext, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()
		return httpServer.Shutdown(shutdownContext)
	case err := <-errorsChannel:
		if errors.Is(err, http.ErrServerClosed) {
			return nil
		}
		return err
	}
}

func runHashPassword(args []string, stdin io.Reader, stdout, stderr io.Writer) error {
	flags := flag.NewFlagSet("hash-password", flag.ContinueOnError)
	flags.SetOutput(stderr)
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 0 {
		return errors.New("hash-password reads one password line from standard input")
	}
	password, err := bufio.NewReader(io.LimitReader(stdin, 130)).ReadString('\n')
	if err != nil && !errors.Is(err, io.EOF) {
		return err
	}
	password = strings.TrimSuffix(strings.TrimSuffix(password, "\n"), "\r")
	if len(password) < 8 || len(password) > 128 {
		return errors.New("password must be 8 to 128 bytes")
	}
	hash, err := auth.Hash(password, auth.DefaultParams())
	if err != nil {
		return err
	}
	_, err = fmt.Fprintln(stdout, hash)
	return err
}

func runBackup(ctx context.Context, args []string, stderr io.Writer) error {
	flags := flag.NewFlagSet("backup", flag.ContinueOnError)
	flags.SetOutput(stderr)
	database := flags.String("database", envOr("AEGISUB_COLLAB_DATABASE", "collab.db"), "SQLite database path")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if flags.NArg() != 1 {
		return errors.New("usage: backup [-database path] destination")
	}
	return collab.BackupDatabase(ctx, *database, flags.Arg(0))
}

func runRooms(ctx context.Context, args []string, stdout, stderr io.Writer) error {
	if len(args) == 0 {
		return errors.New("usage: rooms stats|archive|unarchive")
	}
	flags := flag.NewFlagSet("rooms "+args[0], flag.ContinueOnError)
	flags.SetOutput(stderr)
	database := flags.String("database", envOr("AEGISUB_COLLAB_DATABASE", "collab.db"), "SQLite database path")
	if err := flags.Parse(args[1:]); err != nil {
		return err
	}
	switch args[0] {
	case "stats":
		if flags.NArg() != 0 {
			return errors.New("usage: rooms stats [-database path]")
		}
		stats, err := collab.RoomStats(ctx, *database)
		if err != nil {
			return err
		}
		encoder := json.NewEncoder(stdout)
		encoder.SetIndent("", "  ")
		return encoder.Encode(stats)
	case "archive", "unarchive":
		if flags.NArg() != 1 {
			return fmt.Errorf("usage: rooms %s [-database path] room-name", args[0])
		}
		return collab.SetRoomArchived(ctx, *database, flags.Arg(0), args[0] == "archive")
	default:
		return errors.New("usage: rooms stats|archive|unarchive")
	}
}

func envOr(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}

func usageError() error {
	return errors.New("usage: aegisub-together-server serve|hash-password|backup|rooms")
}
