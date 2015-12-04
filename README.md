# CalcGraph {#mainpage}

A lightweight C++14 header-only framework for organising application logic to minimise end-to-end calculation latency. This is designed for many frequently-updated inputs and allows you to trade off processing each input value against ensuring the application logic output reflects the most recent inputs.

## Examples

## Getting Started

## Dependencies

CalcGraph uses boost intrusive_ptr, a header-only smart pointer library. The tests use [cppunit](http://sourceforge.net/projects/cppunit).

## Contributing

Set up the `clang-format` git filter used by the `.gitattributes` to invoke clang-format by running:

```
$ git config filter.clang-format.clean clang-format
$ git config filter.clang-format.smudge cat
```
