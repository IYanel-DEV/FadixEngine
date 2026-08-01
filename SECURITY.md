# Security Policy

## Reporting a vulnerability

Fadix Engine is an early-preview project, but security issues are still taken
seriously. If you find a vulnerability, please **do not open a public issue**.
Instead, report it privately by emailing the maintainer at the address listed
on the repository owner profile.

Please include:

- A description of the vulnerability and its impact.
- Steps to reproduce it.
- The affected version(s) and build configuration.
- Any relevant crash logs, output, or minimal repro project.

You should receive an acknowledgment within a few days. Please allow time for
the issue to be investigated before disclosing it publicly.

## Supported versions

Only the latest release is actively patched for security issues. Releases are
tagged on the `main` branch from the `dev` line; the most recent tag is the
supported version.

## Reporting process

1. Vulnerability reports are triaged by the maintainers.
2. A fix is prepared and verified against the release CI workflow.
3. A patched release is published, and the report is acknowledged in the
   release notes.
4. The issue is only disclosed publicly after the fix is released.
