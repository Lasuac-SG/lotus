# APA DataFlow Unit Tests

add_lotus_test_suite(apa_tests
  SOURCES
    EliminationTest.cpp
    AffineRelationDomainTest.cpp
    InterAffineEqualitiesTest.cpp
    InterSummaryTransferTest.cpp
    PathSummaryEquationSolverTest.cpp
    OrderTest.cpp
    EAN/RoundtripTest.cpp
    EAN/FactorizeTest.cpp
    EAN/SaturateTest.cpp
    EAN/BatchExtractTest.cpp
    EAN/StarTest.cpp
    EAN/SolverWiringTest.cpp
    EAN/EvalRq1Test.cpp
    EAN/EvalExportTest.cpp
  LINK_LIBS APADataFlow LotusEGraph)
