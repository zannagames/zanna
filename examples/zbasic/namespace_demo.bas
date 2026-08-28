REM ============================================================================
REM Namespace Demo — Track A Features
REM ============================================================================
REM
REM This program demonstrates:
REM   1. NAMESPACE declarations (nested and merged)
REM   2. USING directives (simple import)
REM   3. Cross-namespace inheritance
REM   4. Case-insensitive namespace lookups
REM   5. Multi-level nested namespaces
REM
REM Expected output:
REM   Namespace demo compiled successfully
REM ============================================================================

REM USING directives must come first (before any NAMESPACE declarations)
USING Graphics.Rendering
USING Graphics.Core

REM ============================================================================
REM Define nested namespace: Graphics.Rendering
REM ============================================================================

NAMESPACE Graphics.Rendering
  CLASS Renderer
    DIM width AS I64
    DIM height AS I64
  END CLASS
END NAMESPACE

REM ============================================================================
REM Merged namespace: Graphics.Rendering (contributing Camera class)
REM ============================================================================

NAMESPACE Graphics.Rendering
  CLASS Camera
    DIM position AS I64
  END CLASS
END NAMESPACE

REM ============================================================================
REM Define another namespace: Graphics.Core
REM ============================================================================

NAMESPACE Graphics.Core
  CLASS Engine
    DIM fps AS I64
  END CLASS
END NAMESPACE

REM ============================================================================
REM Define namespace with cross-namespace inheritance
REM ============================================================================

NAMESPACE Application
  REM Inherit from Graphics.Rendering.Renderer using fully-qualified name
  CLASS GameRenderer : Graphics.Rendering.Renderer
    DIM vsync AS I64
  END CLASS

  REM Inherit from Graphics.Core.Engine using fully-qualified name
  CLASS GameEngine : Graphics.Core.Engine
    DIM running AS I64
  END CLASS
END NAMESPACE

REM ============================================================================
REM Multi-level nested namespace
REM ============================================================================

NAMESPACE Company.Product.Module
  CLASS Component
    DIM version AS I64
  END CLASS
END NAMESPACE

REM ============================================================================
REM Case-insensitive demonstration
REM ============================================================================

NAMESPACE Testing
  REM All of these inherit from the same type (case-insensitive)
  CLASS TestA : graphics.rendering.Renderer
    DIM id AS I64
  END CLASS

  CLASS TestB : GRAPHICS.RENDERING.RENDERER
    DIM id AS I64
  END CLASS

  CLASS TestC : Graphics.Rendering.Renderer
    DIM id AS I64
  END CLASS
END NAMESPACE

PRINT "Namespace demo compiled successfully"
END
