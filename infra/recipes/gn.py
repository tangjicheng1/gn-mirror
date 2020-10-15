# Copyright 2018 The Chromium Authors. All rights reserved.
# Use of this source code is governed under the Apache License, Version 2.0
# that can be found in the LICENSE file.
"""Recipe for building GN."""

from recipe_engine.recipe_api import Property

DEPS = [
    'recipe_engine/buildbucket',
    'recipe_engine/cipd',
    'recipe_engine/context',
    'recipe_engine/file',
    'recipe_engine/json',
    'recipe_engine/path',
    'recipe_engine/platform',
    'recipe_engine/properties',
    'recipe_engine/python',
    'recipe_engine/raw_io',
    'recipe_engine/step',
    'target',
    'macos_sdk',
    'windows_sdk',
]

PROPERTIES = {
    'repository': Property(kind=str, default='https://gn.googlesource.com/gn'),
}

# On select platforms, link the GN executable against rpmalloc for a small 10% speed boost.
_RPMALLOC_GIT_URL = 'https://fuchsia.googlesource.com/third_party/github.com/mjansson/rpmalloc'
_RPMALLOC_REVISION = '9c030f7a5efed87e3d036440e753c31342f9f909'

# Used to convert os and arch strings to rpmalloc format
_RPMALLOC_MAP = {
  'amd64': 'x86-64',
  'mac': 'macos',
}

def _get_compilation_environment(api, target, cipd_dir):
  if target.is_linux:
    triple = '--target=%s' % target.triple
    sysroot = '--sysroot=%s' % cipd_dir.join('sysroot')
    env = {
        'CC': cipd_dir.join('bin', 'clang'),
        'CXX': cipd_dir.join('bin', 'clang++'),
        'AR': cipd_dir.join('bin', 'llvm-ar'),
        'CFLAGS': '%s %s' % (triple, sysroot),
        'LDFLAGS': '%s %s -static-libstdc++' % (triple, sysroot),
    }
  elif target.is_mac:
    triple = '--target=%s' % target.triple
    sysroot = '--sysroot=%s' % api.step(
        'xcrun', ['xcrun', '--show-sdk-path'],
        stdout=api.raw_io.output(
            name='sdk-path', add_output_log=True),
        step_test_data=lambda: api.raw_io.test_api.stream_output(
            '/some/xcode/path')).stdout.strip()
    stdlib = cipd_dir.join('lib', 'libc++.a')
    env = {
        'CC': cipd_dir.join('bin', 'clang'),
        'CXX': cipd_dir.join('bin', 'clang++'),
        'AR': cipd_dir.join('bin', 'llvm-ar'),
        'CFLAGS': '%s %s' % (triple, sysroot),
        'LDFLAGS': '%s %s -nostdlib++ %s' % (triple, sysroot, stdlib),
    }
  else:
    env = {}

  return env


def RunSteps(api, repository):
  src_dir = api.path['start_dir'].join('gn')

  # TODO: Verify that building and linking rpmalloc works on OS X and Windows as
  # well.
  use_rpmalloc = api.platform.is_linux
  with api.step.nest('git'), api.context(infra_steps=True):
    api.step('init', ['git', 'init', src_dir])

    with api.context(cwd=src_dir):
      build_input = api.buildbucket.build_input
      ref = (
          build_input.gitiles_commit.id
          if build_input.gitiles_commit else 'refs/heads/master')
      # Fetch tags so `git describe` works.
      api.step('fetch', ['git', 'fetch', '--tags', repository, ref])
      api.step('checkout', ['git', 'checkout', 'FETCH_HEAD'])
      revision = api.step(
          'rev-parse', ['git', 'rev-parse', 'HEAD'],
          stdout=api.raw_io.output()).stdout.strip()
      for change in build_input.gerrit_changes:
        api.step('fetch %s/%s' % (change.change, change.patchset), [
            'git', 'fetch', repository,
            'refs/changes/%s/%s/%s' %
            (str(change.change)[-2:], change.change, change.patchset)
        ])
        api.step('cherry-pick %s/%s' % (change.change, change.patchset),
                 ['git', 'cherry-pick', 'FETCH_HEAD'])

  with api.context(infra_steps=True):
    cipd_dir = api.path['start_dir'].join('cipd')
    pkgs = api.cipd.EnsureFile()
    pkgs.add_package('infra/ninja/${platform}', 'version:1.8.2')
    if api.platform.is_linux or api.platform.is_mac:
      pkgs.add_package('fuchsia/third_party/clang/${platform}', 'integration')
    if api.platform.is_linux:
      pkgs.add_package('fuchsia/third_party/sysroot/linux',
                       'git_revision:c912d089c3d46d8982fdef76a50514cca79b6132',
                       'sysroot')
    api.cipd.ensure(cipd_dir, pkgs)

  # The order is important since release build will get uploaded to CIPD.
  configs = [
      {
          'name': 'debug',
          'args': ['-d'],
          'targets': [api.target.host],
      },
      {
          'name': 'release',
          'args': ['--use-lto', '--use-icf'],
          'targets': [api.target('linux-amd64'),
                      api.target('linux-arm64')]
                     if api.platform.is_linux else [api.target.host],
      },
  ]

  with api.macos_sdk(), api.windows_sdk():
    # Build the rpmalloc stastic libraries if needed.
    if use_rpmalloc:
      # Maps a target.platform string to the location of the corresponding
      # rpmalloc static library.
      rpmalloc_static_libs = {}

      # Get the list of all target platforms that are listed in `configs`
      # above. Note that this is a list of Target instances, some of them
      # may refer to the same platform string (e.g. linux-amd64).
      #
      # For each platform, a version of rpmalloc will be built if necessary,
      # but doing this properly requires having a valid target instance to
      # call _get_compilation_environment. So create a { platform -> Target }
      # map to do that later.
      all_config_platforms = {}
      for c in configs:
        for t in c['targets']:
          if t.platform not in all_config_platforms:
            all_config_platforms[t.platform] = t

      rpmalloc_src_dir = api.path['start_dir'].join('rpmalloc')
      with api.step.nest('rpmalloc'):
        api.step('init', ['git', 'init', rpmalloc_src_dir])
        with api.context(cwd=rpmalloc_src_dir, infra_steps=True):
          api.step(
              'fetch',
              ['git', 'fetch', '--tags', _RPMALLOC_GIT_URL, _RPMALLOC_REVISION])
          api.step('checkout', ['git', 'checkout', 'FETCH_HEAD'])

        for platform in all_config_platforms:
          # Convert target architecture and os to rpmalloc format.
          rpmalloc_os, rpmalloc_arch = platform.split('-')
          rpmalloc_os = _RPMALLOC_MAP.get(rpmalloc_os, rpmalloc_os)
          rpmalloc_arch = _RPMALLOC_MAP.get(rpmalloc_arch, rpmalloc_arch)

          # The rpmalloc build system doesn't support out-of-tree builds.
          # So copy the source tree into a new clean directory.
          rpmalloc_build_dir = api.path['cleanup'].join('rpmalloc-' + platform)
          api.file.rmtree('remove sources ' + platform, rpmalloc_build_dir)
          api.file.copytree('copy sources ' + platform, rpmalloc_src_dir, rpmalloc_build_dir)

          env = _get_compilation_environment(api, all_config_platforms[platform], cipd_dir)
          with api.step.nest('build rpmalloc-' + platform), api.context(
              env=env, cwd=rpmalloc_build_dir):
            api.python(
                'configure',
                rpmalloc_build_dir.join('configure.py'),
                args=['-c', 'release', '-a', rpmalloc_arch, '--lto'])

            # NOTE: Only build the static library.
            rpmalloc_static_lib = api.path.join('lib', rpmalloc_os, 'release', rpmalloc_arch, 'librpmallocwrap.a')
            api.step('ninja', [cipd_dir.join('ninja'), rpmalloc_static_lib])

          rpmalloc_static_libs[platform] = rpmalloc_build_dir.join(rpmalloc_static_lib)

    for config in configs:
      with api.step.nest(config['name']):
        for target in config['targets']:
          env = _get_compilation_environment(api, target, cipd_dir)
          with api.step.nest(target.platform):
            with api.context(env=env, cwd=src_dir):
              args = config['args']

              # Ensure that the release `gn` binary will be linked to the rpmalloc static
              # library when needed.
              if use_rpmalloc and config['name'] == 'release':
                args = args[:] + ['--link-lib=%s' % rpmalloc_static_libs[target.platform]]

              api.python(
                  'generate',
                  src_dir.join('build', 'gen.py'),
                  args=args)

              # Windows requires the environment modifications when building too.
              api.step('build',
                       [cipd_dir.join('ninja'), '-C',
                        src_dir.join('out')])

            if target.is_host:
              api.step('test', [src_dir.join('out', 'gn_unittests')])

            if build_input.gerrit_changes:
              continue

            if config['name'] != 'release':
              continue

            with api.step.nest('upload'):
              cipd_pkg_name = 'gn/gn/%s' % target.platform
              gn = 'gn' + ('.exe' if target.is_win else '')

              pkg_def = api.cipd.PackageDefinition(
                  package_name=cipd_pkg_name,
                  package_root=src_dir.join('out'),
                  install_mode='copy')
              pkg_def.add_file(src_dir.join('out', gn))
              pkg_def.add_version_file('.versions/%s.cipd_version' % gn)

              cipd_pkg_file = api.path['cleanup'].join('gn.cipd')

              api.cipd.build_from_pkg(
                  pkg_def=pkg_def,
                  output_package=cipd_pkg_file,
              )

              if api.buildbucket.builder_id.project == 'infra-internal':
                cipd_pin = api.cipd.search(cipd_pkg_name,
                                           'git_revision:' + revision)
                if cipd_pin:
                  api.step('Package is up-to-date', cmd=None)
                  continue

                api.cipd.register(
                    package_name=cipd_pkg_name,
                    package_path=cipd_pkg_file,
                    refs=['latest'],
                    tags={
                        'git_repository': repository,
                        'git_revision': revision,
                    },
                )


def GenTests(api):
  for platform in ('linux', 'mac', 'win'):
    yield (api.test('ci_' + platform) + api.platform.name(platform) +
           api.buildbucket.ci_build(
               project='gn',
               git_repo='gn.googlesource.com/gn',
           ))

    yield (api.test('cq_' + platform) + api.platform.name(platform) +
           api.buildbucket.try_build(
               project='gn',
               git_repo='gn.googlesource.com/gn',
           ))

  yield (api.test('cipd_exists') + api.buildbucket.ci_build(
      project='infra-internal',
      git_repo='gn.googlesource.com/gn',
      revision='a' * 40,
  ) + api.step_data(
      'git.rev-parse', api.raw_io.stream_output('a' * 40)
  ) + api.step_data(
      'release.linux-amd64.upload.cipd search gn/gn/linux-amd64 git_revision:' +
      'a' * 40,
      api.cipd.example_search('gn/gn/linux-amd64',
                              ['git_revision:' + 'a' * 40])))

  yield (api.test('cipd_register') + api.buildbucket.ci_build(
      project='infra-internal',
      git_repo='gn.googlesource.com/gn',
      revision='a' * 40,
  ) + api.step_data(
      'git.rev-parse', api.raw_io.stream_output('a' * 40)
  ) + api.step_data(
      'release.linux-amd64.upload.cipd search gn/gn/linux-amd64 git_revision:' +
      'a' * 40, api.cipd.example_search('gn/gn/linux-amd64', [])))
