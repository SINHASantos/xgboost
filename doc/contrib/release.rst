.. _release:

XGBoost Release Policy
=======================

Versioning Policy
-----------------

Starting from XGBoost 1.0.0, each XGBoost release will be versioned as [MAJOR].[FEATURE].[MAINTENANCE]

* MAJOR: We guarantee the API compatibility across releases with the same major version number. We expect to have a 1+ years development period for a new MAJOR release version.
* FEATURE: We ship new features, improvements and bug fixes through feature releases. The cycle length of a feature is decided by the size of feature roadmap. The roadmap is decided right after the previous release.
* MAINTENANCE: Maintenance version only contains bug fixes. This type of release only occurs when we found significant correctness and/or performance bugs and barrier for users to upgrade to a new version of XGBoost smoothly.


Making a Release
-----------------

1. Create an issue for the release, noting the estimated date and expected features or major fixes, pin that issue.
2. Create a release branch if this is a major release. Bump release version. There's a helper script ``ops/script/change_version.py``.
3. Commit the change, create a PR on GitHub on release branch.  Port the bumped version to default branch, optionally with the postfix ``SNAPSHOT``.
4. Create a tag on release branch, either on GitHub or locally.
5. Make a release on GitHub tag page, which might be done with previous step if the tag is created on GitHub.
6. Submit pip, R-universe, CRAN, and Maven packages.

   There are helper scripts for automating the process in ``xgboost/dev/``.

   + The pip package is maintained by `Hyunsu Cho <https://github.com/hcho3>`__ and `Jiaming Yuan <https://github.com/trivialfis>`__.

   + The CRAN package and the R-universe packages are maintained by `Jiaming Yuan <https://github.com/trivialfis>`__.

   + The Maven package is maintained by `Nan Zhu <https://github.com/CodingCat>`_ and `Hyunsu Cho <https://github.com/hcho3>`_.


R Universe Packages
-------------------

Since XGBoost 3.0.0, we host the R package on `R-Universe
<https://dmlc.r-universe.dev/xgboost>`__. To make a new release, change the
``packages.json`` in `dmlc.r-universe.dev <https://github.com/dmlc/dmlc.r-universe.dev>`__
with a new release branch.

R CRAN Package
--------------
Before submitting a release, one should test the package on `R-hub <https://r-hub.github.io/rhub/>`__ and `win-builder <https://win-builder.r-project.org/>`__ first.  Please note that the R-hub Windows instance doesn't have the exact same environment as the one hosted on win-builder.

According to the `CRAN policy <https://cran.r-project.org/web/packages/policies.html>`__:

    If running a package uses multiple threads/cores it must never use more than two simultaneously: the check farm is a shared resource and will typically be running many checks simultaneously.

We need to check the number of CPUs used in examples. Export ``_R_CHECK_EXAMPLE_TIMING_CPU_TO_ELAPSED_THRESHOLD_=2.5`` before running ``R CMD check --as-cran`` `[1] <#references>`__ and make sure the machine you are using has enough CPU cores to reveal any potential policy violation.

Read The Docs
-------------

We might need to manually activate the new release branch for `read the docs
<https://xgboost.readthedocs.io/>`__ and set it as the default branch in the console `[2]
<#references>`__. Please check the document build and make sure the correct branch is
activated and selected after making a new release.

References
----------

[1] https://stat.ethz.ch/pipermail/r-package-devel/2022q4/008610.html

[2] https://github.com/readthedocs/readthedocs.org/issues/12073