# NPA DataFlow Unit Tests

add_lotus_test_suite(npa_tests
  SOURCES
    NPASolverTest.cpp
    NPASymmetricAnalysesTest.cpp
    NPAAdaptiveSccTest.cpp
    TensorProductLiftTest.cpp
    NPATensorRegularizationTest.cpp
    Interprocedural/Client/NPAInterproceduralClientTest.cpp
    PredicateRelationDomainTest.cpp
    Interprocedural/Taint/NPAInterproceduralTaintTest.cpp
    Interprocedural/NPAInterproceduralNullabilityTest.cpp
    NPAThreadSafetyHardeningTest.cpp
  LINK_LIBS
    NPADataFlow
    CanaryAliasAnalysisWrapper)
