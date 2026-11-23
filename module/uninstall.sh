#
# Copyright (C) 2024-2026 Rem01Gaming
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Put back the gperf knob.txt that was there before a game session
if [ -f /data/adb/.config/encore/gperf_active ]; then
	rm -f /data/vendor/exynos-profiler/knob.txt
	[ -f /data/vendor/exynos-profiler/knob.txt.encore-bak ] &&
		mv -f /data/vendor/exynos-profiler/knob.txt.encore-bak /data/vendor/exynos-profiler/knob.txt
fi

rm -rf /data/adb/.config/encore
rm -f /data/adb/service.d/.encore_cleanup.sh

need_gone="encored encore_profiler encore_utility encore_log"
manager_paths="/data/adb/ap/bin /data/adb/ksu/bin"

for dir in $manager_paths; do
	[ -d "$dir" ] && {
		for bin in $need_gone; do
			rm "$dir/$bin"
		done
	}
done
