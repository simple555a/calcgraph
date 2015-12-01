# CalcGraph

## Contributing

Set up the `clang-format` git filter used by the `.gitattributes` to invoke clang-format by running:

````
$ git config filter.clang-format.clean clang-format
$ git config filter.clang-format.smudge cat
````
