#!/usr/bin/env bash
# Hand the display over to a fake greetd and check that the switch waits for the shell's background layer.
set -euo pipefail
python3 "$UMBRIEL_REPO/tests/session_handover.py" "$UMBRIEL" "$UMBRIEL_LAYER_CLIENT"
