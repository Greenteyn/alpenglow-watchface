#
# This file is the default set of rules to compile a Pebble application.
#
# Feel free to customize this to your needs.
#
import os
import os.path
import re

try:
    from waflib import Logs
except ImportError:  # without waflib the warning simply goes to stdout
    Logs = None

top = '.'
out = 'build'

# The debug location override in pkjs must never reach a release (see the comment
# at DEBUG_LOCATION in src/pkjs/index.js). Only the top-level declaration is
# matched, so mentions in comments or console.log do not count as an active
# override.
DEBUG_LOCATION_RE = re.compile(r'^var\s+DEBUG_LOCATION\s*=\s*([^;]+);', re.M)


def check_debug_location(ctx):
    """Warn about an overridden location; fail the build when RELEASE=1."""
    node = ctx.path.find_node('src/pkjs/index.js')
    if node is None:
        return

    match = DEBUG_LOCATION_RE.search(node.read())
    if match is None:
        return

    value = match.group(1).strip()
    if value == 'null':
        return

    msg = ('DEBUG_LOCATION = {} in src/pkjs/index.js — the location is '
           'overridden and real geolocation is not queried'.format(value))

    if os.environ.get('RELEASE') == '1':
        ctx.fatal('RELEASE=1: ' + msg + '. Set DEBUG_LOCATION = null.')

    warning = ('*** ' + msg + '. Set it back to null before a release; '
               'check with: RELEASE=1 pebble build')
    if Logs is not None:
        Logs.warn(warning)
    else:
        print(warning)


def options(ctx):
    ctx.load('pebble_sdk')


def configure(ctx):
    """
    This method is used to configure your build. ctx.load(`pebble_sdk`) automatically configures
    a build for each valid platform in `targetPlatforms`. Platform-specific configuration: add your
    change after calling ctx.load('pebble_sdk') and make sure to set the correct environment first.
    Universal configuration: add your change prior to calling ctx.load('pebble_sdk').
    """
    ctx.load('pebble_sdk')


def build(ctx):
    ctx.load('pebble_sdk')

    check_debug_location(ctx)

    build_worker = os.path.exists('worker_src')
    binaries = []

    cached_env = ctx.env
    for platform in ctx.env.TARGET_PLATFORMS:
        ctx.env = ctx.all_envs[platform]
        ctx.set_group(ctx.env.PLATFORM_NAME)
        app_elf = '{}/pebble-app.elf'.format(ctx.env.BUILD_DIR)
        ctx.pbl_build(source=ctx.path.ant_glob('src/c/**/*.c'), target=app_elf, bin_type='app')

        if build_worker:
            worker_elf = '{}/pebble-worker.elf'.format(ctx.env.BUILD_DIR)
            binaries.append({'platform': platform, 'app_elf': app_elf, 'worker_elf': worker_elf})
            ctx.pbl_build(source=ctx.path.ant_glob('worker_src/c/**/*.c'),
                          target=worker_elf,
                          bin_type='worker')
        else:
            binaries.append({'platform': platform, 'app_elf': app_elf})
    ctx.env = cached_env

    ctx.set_group('bundle')
    ctx.pbl_bundle(binaries=binaries,
                   js=ctx.path.ant_glob(['src/pkjs/**/*.js',
                                         'src/pkjs/**/*.json',
                                         'src/common/**/*.js']),
                   js_entry_file='src/pkjs/index.js')
