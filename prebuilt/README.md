This folder contains following prebuilt programs:

[Binder Resolver](https://github.com/Rem01Gaming/binder_resolver): Used for retrieving binder transaction number.

[Encore FAS](https://github.com/rem01project/encore_fas): `fas/<kmi>_encore_fas.ko` kernel modules used by FAS (Frame Aware Scheduling). `service.sh` loads the one matching the running kernel's KMI (e.g. `android14-6.1`). Build more KMIs with that repo's `Build LKM` workflow and drop them here.
