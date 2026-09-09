#!/usr/bin/env python3
"""
tier4 libstratosphere patch: non-domain mitm sub-object forwarding.

Upstream libstratosphere only wires a forward service to sub-objects returned
from a mitm command on DOMAIN sessions (sf_hipc_server_domain_session_manager).
On a non-domain session (e.g. a game's vi:u session) SetOutObjectImpl calls
plain RegisterSession -> the sub-session has no forward service -> any command
the wrapper doesn't declare hits ForwardRequest() on a null service -> abort.

This adds a thread-local side channel: a mitm command handler sets
ams::sf::impl::g_tier4_pending_mitm_forward to the forwarded ::Service right
before out.SetValue(); SetOutObjectImpl consumes it and calls RegisterMitmSession
(with forward service) instead. Runs on the same thread / call stack, so the
thread-local is safe.

Idempotent - safe to run every build.
"""
import sys, pathlib

F = pathlib.Path(sys.argv[1]) / "libraries/libstratosphere/include/stratosphere/sf/impl/sf_impl_command_serialization.hpp"
src = F.read_text()

MARK = "g_tier4_pending_mitm_forward"
if MARK in src:
    print("patch_libstrat: already applied")
    sys.exit(0)

anchor = '#include <stratosphere/sf/hipc/sf_hipc_server_session_manager.hpp>\n'
inject = anchor + (
    "\n/* --- tier4 patch: non-domain mitm sub-object forwarding side channel --- */\n"
    "#if AMS_SF_MITM_SUPPORTED\n"
    "#include <memory>\n"
    "namespace ams::sf::impl { inline thread_local ::std::shared_ptr<::Service> g_tier4_pending_mitm_forward{}; }\n"
    "#endif\n"
)
assert anchor in src, "anchor include not found"
src = src.replace(anchor, inject, 1)

old = "                R_ABORT_UNLESS(manager->RegisterSession(server_handle, std::move(object)));\n"
new = (
    "#if AMS_SF_MITM_SUPPORTED\n"
    "                if (auto _t4fwd = std::move(::ams::sf::impl::g_tier4_pending_mitm_forward); _t4fwd != nullptr) {\n"
    "                    R_ABORT_UNLESS(manager->RegisterMitmSession(server_handle, std::move(object), std::move(_t4fwd)));\n"
    "                } else\n"
    "#endif\n"
    "                {\n"
    "                    R_ABORT_UNLESS(manager->RegisterSession(server_handle, std::move(object)));\n"
    "                }\n"
)
assert old in src, "RegisterSession line not found"
src = src.replace(old, new, 1)

F.write_text(src)
print("patch_libstrat: applied")
