# Contributing Guide

Thanks for taking time to contribute!

## Submitting an Error Report

If you discover a vulnerability that does not pose a security risk within msMemScope, first search the msMemScope repository for existing issues to avoid creating duplicates. If the issue is not yet listed, then create a new one. If you discover a security-related problem, do not disclose it publicly. Please refer to the security handling guidelines for details. All error reports must include complete information about the issue.

## Handling Security Issues

For guidance on handling security issues in this project, please contact the core team via email for confirmation.

## Resolving Existing Issues

Review the repository's issue list to identify items requiring attention, and attempt to resolve them.

## Proposing a New Feature

Please label your proposal as **Feature**. We will review and confirm it on a regular basis.

## Starting to Contribute

1. Fork the repository of the project.
2. Clone it to your local machine.
3. Create a development branch.
4. Conduct local testing. All unit tests, including any new test cases, must pass before submission.
5. Submit your code.
6. Create a pull request (PR).
7. Review your code based on review comments and push updates again. This process may involve multiple iterations.
8. After your PR is approved by the required number of reviewers, the committer will conduct the final review.
9. After your PR is approved and all tests pass, the CI system will merge it into the project's main branch.

For details, see [msMemScope Development Guide](./development_guide.md).

## Building and Testing

Before submitting a PR, you are advised to set up a local development environment, build `msMemScope`, and run related tests.
For details about how to set up the development and test environment, see [Setting up the Development and Test Environment](./development_guide.md).

## PR Types

Only PRs of specific types can be reviewed. Add a proper prefix before your PR title to specify the PR type. The proper types include:

- `[feature]`: new module features and basic features
- `[bugfix]`: fixes for project bugs
- `[refactor]`: re-construction of the existing modules
- `[docs]`: new or modified documentation
- `[test]`: new or modified UT/STs
- `[build]`: changes to the build system (CMake, build.py, etc.)

If your PR is in draft state and does not need to be reviewed, use the following label.

- `[WIP]`: draft. No review is needed.

## Commit Requirements

1. Describe code functions in each commit message. Invalid messages, such as "add adaptation file" and "first commit", will not pass the check.
2. Compress unnecessary commits, for example, commits with the same commit messages and consecutive code checks.

If there are multiple commits, compress them into one commit record. (Although GitCode provides the `Squash merge` option for PR merging, it is still considered a best practice to sort PRs into a single concise commit in advance.)

### Method 1: (Recommended) Interactive Rebasing

1. View the latest several commits to be merged, for example, the latest three commits.

    ``` shell
    git log --oneline -n 3
    ```

2. Select the previous commit ID of the commits to be merged and run the following command.

    ```shell
    # # Replace commit_id with the actual ID.
    git rebase -i commit_id
    ```

3. Change `pick` corresponding to the ID of the commit to be compressed to `squash` retain at least one `pick`.
4. Save the modification, exit the current window, and open the second editing environment to adjust commit messages.
5. Change the commit message of the corresponding `pick`.
6. Save the settings and exit.
7. Forcibly push the updated branch (your own branch only).

    ```shell
    # Replace branch_name with the actual branch name.
    git push -f origin branch_name
    ```

8. Check that PR is changed to the required status.

### Method 2: Reset + New Commit

```bash
# Obtain the latest target branch (for example, `main`) to be merged.
git fetch origin main

# Soft-reset to the main branch. This operation saves all modifications and goes back to the staging area.
git reset --soft origin/main

# Commit all changes as a new commit.
git commit -m "feat: concise description of your change"

# Forcibly push the commit to update the PR branch.
git push --force-with-lease origin your-branch-name

```

## PR Merging Process

1. Submit a PR and comment `compile` to trigger CI compilation.
2. Wait until the CI compilation is complete and check whether there are compilation errors.
3. If the compilation is successful, contact the maintenance team to review and merge the PR.
4. The maintenance team reviews the PR. If the PR meets the requirements, the maintenance team merges the PR.

### Contacting the Maintenance Team

If you encounter any problem or need further help during PR merging, contact the maintenance team in the following ways:

- Email: <memscope@outlook.com>
- Group chat: Scan the QR code to add the Ascend open-source assistant, obtain the group link, and join the MindStudio community technical exchange group to obtain help and support. For details, see [Group Chat](../communication_guide/communication.md#3- open-source assistant).

## Submitting an Issue

See [How to Issue](../communication_guide/how_to_issue.md).
