#!/bin/sh
# Starts the permission hook with a Node that is known to work.
#
# Claude Code runs a hook with whatever PATH it was started with, and a terminal and
# an editor do not start it with the same one: VS Code opened from the Dock often
# cannot find `node` at all. A hook that cannot start decides nothing, so the box
# would look dead and nothing would say why.
#
# The daemon is a Node program, so it knows one that works, and leaves its path in
# a file beside its socket. Use that one; failing that, whatever the PATH offers;
# failing that, stand down quietly — Claude's own dialog is in charge, as it is
# whenever the hook has nothing to say.
#
# Nothing here runs an external command before node does: with a bare PATH there
# may be nothing to run.

socket=${CLAUDE_REMOTE_SOCKET:-}
if [ -n "$socket" ]; then
  dir=${socket%/*}
else
  dir=${HOME:-}/.claude-remote
fi

# The file is run, so it is trusted only if it is this user's own and names a path
# outright: a socket kept in a directory others can write to must not let them
# choose what a hook runs. Anything else is as good as no file.
node_bin=
if [ -r "$dir/node" ] && [ -O "$dir/node" ]; then
  read -r node_bin < "$dir/node"
fi
case $node_bin in
  /*) ;;
  *) node_bin= ;;
esac
if [ -z "$node_bin" ] || [ ! -x "$node_bin" ]; then
  node_bin=$(command -v node)
fi
[ -n "$node_bin" ] || exit 0

case $0 in
  */*) here=${0%/*} ;;
  *) here=. ;;
esac

exec "$node_bin" "$here/permission-hook.mjs"
