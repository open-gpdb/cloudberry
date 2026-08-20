# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#  http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

test_anser_sources = files(
  'anser_test.c',
)

if host_system == 'windows'
  test_anser_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_anser',
    '--FILEDESC', 'test_anser - test helpers for Anser',])
endif

test_anser = shared_module('test_anser',
  test_anser_sources,
  kwargs: pg_test_mod_args + {
    'dependencies': pg_test_mod_args['dependencies'] + [libpq],
  },
)
test_install_libs += test_anser

test_install_data += files(
  'test_anser.control',
  'test_anser--1.0.sql',
)

tests += {
  'name': 'test_anser',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_anser',
      'anser_runtime_filter',
    ],
  },
}
