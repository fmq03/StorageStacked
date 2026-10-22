# How to Contribute

We'd love to accept your patches and contributions to this project. There are
just a few small guidelines you need to follow.

## Sign our Contributor License Agreement

Contributions to this project must be accompanied by a
[Contributor License Agreement](https://cla.developers.google.com/about) (CLA).
You (or your employer) retain the copyright to your contribution; this simply
gives us permission to use and redistribute your contributions as part of the
project.

If you or your current employer have already signed the Google CLA (even if it
was for a different project), you probably don't need to do it again.

Visit <https://cla.developers.google.com/> to see your current agreements or to
sign a new one.

## Review our community guidelines

This project follows
[Google's Open Source Community Guidelines](https://opensource.google/conduct/).

## Code Reviews

All submissions, including submissions by project members, require review. We
use GitHub pull requests for this purpose. Consult
[GitHub Help](https://help.github.com/articles/about-pull-requests/) for more
information on using pull requests.

## Lint and Quality Checks

Before sending a pull request, run the following verification scripts:

- **Linters**: Run `utils/run_linters.sh` to check formatting.
- **Macro Signatures**: Run `utils/check_macro_signatures.py` to ensure
    parameter parity between the regular and split VCS simulation macros.

You can configure git to run these quality checks automatically before pushing
by running the following command from the project root:

```bash
git config core.hooksPath .githooks
```
