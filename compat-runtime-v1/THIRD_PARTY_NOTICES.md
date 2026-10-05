# Third-party notices

## Unicorn Engine

Android builds of `compat-runtime-v1` fetch Unicorn Engine 2.1.4 from the upstream `unicorn-engine/unicorn` repository at commit `8028ec436f2d9376525352dd38ed9ed6b9f6be10`, and build only its ARM backend. Unicorn is distributed under the GNU Lesser General Public License, version 2.1 or later. Its license text is available in the upstream source tree as `COPYING.LGPL2` and at <https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html>.

The Android runtime links the backend as a shared `libunicorn.so`; retain the corresponding license and modification notices when redistributing a build. No Unicorn source or binary is vendored in this repository.
