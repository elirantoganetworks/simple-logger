# Security Policy

## Supported versions

slog uses semantic versioning. Security fixes go to the latest 1.x release.

| Version | Supported |
|---------|-----------|
| 1.x     | Yes       |
| < 1.0   | No        |

## Reporting a vulnerability

Please report security issues by email to eliran.toganetworks@gmail.com.

Do not open a public issue for a security problem.

Include:

- what the problem is,
- the version and platform,
- steps to reproduce, and
- the impact you expect.

## What to expect

- You get a reply within 5 working days.
- If the report is valid, we agree on a fix and a release date with you.
- We credit you in the release notes if you want.
- Please give us a chance to ship a fix before you make the issue public.

## Scope

slog is a small C++ logging library for Linux. The parts most relevant to
security are:

- It writes to files and directories under a log dir that the program chooses.
  A program that points the log dir at an untrusted or shared location, or runs
  with more rights than it needs, can expose those files.
- Log messages may hold data from users or the network. slog turns any newline
  or carriage return in a message into a space, so a message cannot forge extra
  log lines. It does not otherwise filter or escape message content.
- The optional crash handler installs signal handlers. It only does async
  signal safe work (a raw write and an fdatasync), then re-raises the signal.

Out of scope: what your program logs, where you point the log dir, and the
permissions your program runs with. Those are the caller's choice.
