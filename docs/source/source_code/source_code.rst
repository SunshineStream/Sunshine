Source Code
===========
We are in process of improving the source code documentation. Code should be documented using Doxygen syntax.
Some examples exist in `main.h` and `main.cpp`. In order for documentation within the code to appear in the
rendered docs, the definition of the object must be in a header file, although the documentation itself can (and
should) be in the source file.

Example Documentation Blocks
----------------------------

**file.h**

.. code-block:: c

   // functions
   int main(int argc, char *argv[]);

**file.cpp** (with markdown)

.. code-block:: cpp

   /**
    * @brief Main application entry point.
    * @param argc The number of arguments.
    * @param argv The arguments.
    *
    * EXAMPLES:
    * ```cpp
    * main(1, const char* args[] = {"hello", "markdown", nullptr});
    * ```
    */
   int main(int argc, char *argv[]) {
     // do stuff
   }

**file.cpp** (with ReStructuredText)

.. code-block:: cpp

   /**
    * @brief Main application entry point.
    * @param argc The number of arguments.
    * @param argv The arguments.
    * @rst
    * EXAMPLES:
    *
    * .. code-block:: cpp
    *    main(1, const char* args[] = {"hello", "rst", nullptr});
    * @endrst
    */
   int main(int argc, char *argv[]) {
     // do stuff
   }

Source
------

.. toctree::
   :caption: src
   :maxdepth: 1
   :glob:

   src/*

.. toctree::
   :caption: src/display_device
   :maxdepth: 1
   :glob:

   src/display_device/*

.. toctree::
   :caption: src/platform
   :maxdepth: 1
   :glob:

   src/platform/*

.. toctree::
   :caption: src/platform/linux
   :maxdepth: 1
   :glob:

   src/platform/linux/*

.. toctree::
   :caption: src/platform/macos
   :maxdepth: 1
   :glob:

   src/platform/macos/*

.. toctree::
   :caption: src/platform/windows
   :maxdepth: 1
   :glob:

   src/platform/windows/*
