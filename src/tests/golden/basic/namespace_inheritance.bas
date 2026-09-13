REM ============================================================================
REM namespace_inheritance.bas
REM Purpose: Demonstrate cross-namespace class inheritance.
REM Track A: Classes can inherit from types in other namespaces using
REM          fully-qualified names (NS.Type syntax)
REM ============================================================================

NAMESPACE Foundation
  CLASS BaseEntity
    DIM id AS I64
    DIM name AS STRING
  END CLASS
END NAMESPACE

NAMESPACE Application.Domain
  REM Inherit from Foundation.BaseEntity using fully-qualified name
  CLASS Customer : Foundation.BaseEntity
    DIM email AS STRING
    DIM phone AS STRING
  END CLASS
END NAMESPACE

NAMESPACE Application.Services
  REM Another class inheriting from a different namespace
  CLASS Order : Foundation.BaseEntity
    DIM total AS I64
  END CLASS
END NAMESPACE

PRINT "Cross-namespace inheritance compiled successfully"
END
