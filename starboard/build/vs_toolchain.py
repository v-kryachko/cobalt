#!/usr/bin/env python3


import collections
import os
import sys

from build.gn_helpers import ToGNString

SDK_VERSION = '10.0.18362.0'
MSVS_VC_SUBDIR = ['VC', 'Tools', 'MSVC']

script_dir = os.path.dirname(os.path.realpath(__file__))
json_data_file = os.path.join(script_dir, 'win_toolchain.json')

# VS versions are listed in descending order of priority (highest first).
# The first version is assumed by this script to be the one that is packaged,
# which makes a difference for the arm64 runtime.
MSVS_VERSIONS = collections.OrderedDict([
    ('2022', '17.0'),  # Default and packaged version of Visual Studio.
    ('2019', '16.0'),
    ('2017', '15.0'),
])

# List of preferred VC toolset version based on MSVS
# Order is not relevant for this dictionary.
MSVC_TOOLSET_VERSION = {
    '2022': 'VC143',
    '2019': 'VC142',
    '2017': 'VC141',
}

def _RegistryGetValueUsingWinReg(key, value):
  """Use the _winreg module to obtain the value of a registry key.

  Args:
    key: The registry key.
    value: The particular registry value to read.
  Return:
    contents of the registry key's value, or None on failure.  Throws
    ImportError if _winreg is unavailable.
  """
  import _winreg
  try:
    root, subkey = key.split('\\', 1)
    assert root == 'HKLM'  # Only need HKLM for now.
    with _winreg.OpenKey(_winreg.HKEY_LOCAL_MACHINE, subkey) as hkey:
      return _winreg.QueryValueEx(hkey, value)[0]
  except WindowsError:
    return None


def _RegistryGetValue(key, value):
  try:
    return _RegistryGetValueUsingWinReg(key, value)
  except ImportError:
    raise Exception('The python library _winreg not found.')


def GetVisualStudioVersion():
  """Return best available version of Visual Studio.
  """
  supported_versions = list(MSVS_VERSIONS.keys())

  # VS installed in system for external developers
  supported_versions_str = ', '.join('{} ({})'.format(v,k)
      for k,v in MSVS_VERSIONS.items())
  available_versions = []
  for version in supported_versions:
    # Checking vs%s_install environment variables.
    # For example, vs2019_install could have the value
    # "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community".
    # Only vs2017_install, vs2019_install and vs2022_install are supported.
    path = os.environ.get('vs%s_install' % version)
    if path and os.path.exists(path):
      available_versions.append(version)
      break
    # Detecting VS under possible paths.
    if version >= '2022':
      program_files_path_variable = '%ProgramFiles%'
    else:
      program_files_path_variable = '%ProgramFiles(x86)%'
    path = os.path.expandvars(program_files_path_variable +
                              '/Microsoft Visual Studio/%s' % version)
    if path and any(
        os.path.exists(os.path.join(path, edition))
        for edition in ('Enterprise', 'Professional', 'Community', 'Preview',
                        'BuildTools')):
      available_versions.append(version)
      break

  if not available_versions:
    raise Exception('No supported Visual Studio can be found.'
                    ' Supported versions are: %s.' % supported_versions_str)
  return available_versions[0]


def NormalizePath(path):
  while path.endswith('\\'):
    path = path[:-1]
  return path

def DetectVisualStudioPath():
  """Return path to the installed Visual Studio.
  """

  # Note that this code is used from
  # build/toolchain/win/setup_toolchain.py as well.
  version_as_year = GetVisualStudioVersion()

  # The VC++ >=2017 install location needs to be located using COM instead of
  # the registry. For details see:
  # https://blogs.msdn.microsoft.com/heaths/2016/09/15/changes-to-visual-studio-15-setup/
  # For now we use a hardcoded default with an environment variable override.
  if version_as_year >= '2022':
    program_files_path_variable = '%ProgramFiles%'
  else:
    program_files_path_variable = '%ProgramFiles(x86)%'
  for path in (
               os.path.expandvars(program_files_path_variable +
                                  '/Microsoft Visual Studio/%s/Enterprise' %
                                  version_as_year),
               os.path.expandvars(program_files_path_variable +
                                  '/Microsoft Visual Studio/%s/Professional' %
                                  version_as_year),
               os.path.expandvars(program_files_path_variable +
                                  '/Microsoft Visual Studio/%s/Community' %
                                  version_as_year),
               os.path.expandvars(program_files_path_variable +
                                  '/Microsoft Visual Studio/%s/Preview' %
                                  version_as_year),
               os.path.expandvars(program_files_path_variable +
                                  '/Microsoft Visual Studio/%s/BuildTools' %
                                  version_as_year)):
    if path and os.path.exists(path):
      return path

  raise Exception('Visual Studio Version %s not found.' % version_as_year)


def GetToolchainDir():
  """Gets location information about the current toolchain (must have been
  previously updated by 'update'). This is used for the GN build."""
  vspath = DetectVisualStudioPath()
  vc_versions = os.listdir(os.path.join(*([vspath] + MSVS_VC_SUBDIR)))

  print(f'''
vs_path = {ToGNString(NormalizePath(vspath))}
vc_version = {ToGNString(NormalizePath(vc_versions[-1]))}
sdk_path = {ToGNString(os.path.expandvars('%ProgramFiles(x86)%'
                               '/Windows Kits/10'))}
sdk_version = {ToGNString(SDK_VERSION)}
vs_version = {ToGNString(GetVisualStudioVersion())}
  ''')


if __name__ == '__main__':
  sys.exit(GetToolchainDir())
