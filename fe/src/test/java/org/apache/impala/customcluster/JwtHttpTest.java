// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

package org.apache.impala.customcluster;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import java.io.File;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

import org.apache.hive.service.rpc.thrift.*;
import org.apache.impala.util.Metrics;
import org.apache.thrift.transport.THttpClient;
import org.apache.thrift.protocol.TBinaryProtocol;
import org.junit.Before;
import org.junit.Test;

/**
 * Tests that hiveserver2 operations over the http interface work as expected when
 * JWT authentication is being used.
 */
public class JwtHttpTest {
  Metrics metrics = new Metrics();

  public void setUp(String extraArgs) throws Exception {
    int ret = CustomClusterRunner.StartImpalaCluster(extraArgs);
    assertEquals(ret, 0);
  }

  static void verifySuccess(TStatus status) throws Exception {
    if (status.getStatusCode() == TStatusCode.SUCCESS_STATUS
        || status.getStatusCode() == TStatusCode.SUCCESS_WITH_INFO_STATUS) {
      return;
    }
    throw new Exception(status.toString());
  }

  /**
   * Executes 'query' and fetches the results. Expects there to be exactly one string
   * returned, which be be equal to 'expectedResult'.
   */
  static TOperationHandle execAndFetch(TCLIService.Iface client,
      TSessionHandle sessionHandle, String query, String expectedResult)
      throws Exception {
    TExecuteStatementReq execReq = new TExecuteStatementReq(sessionHandle, query);
    TExecuteStatementResp execResp = client.ExecuteStatement(execReq);
    verifySuccess(execResp.getStatus());

    TFetchResultsReq fetchReq = new TFetchResultsReq(
        execResp.getOperationHandle(), TFetchOrientation.FETCH_NEXT, 1000);
    TFetchResultsResp fetchResp = client.FetchResults(fetchReq);
    verifySuccess(fetchResp.getStatus());
    List<TColumn> columns = fetchResp.getResults().getColumns();
    assertEquals(1, columns.size());
    assertEquals(expectedResult, columns.get(0).getStringVal().getValues().get(0));

    return execResp.getOperationHandle();
  }

  private void verifyJwtAuthMetrics(long expectedAuthSuccess, long expectedAuthFailure)
      throws Exception {
    long actualAuthSuccess =
        (long) metrics.getMetric("impala.thrift-server.hiveserver2-http-frontend."
            + "total-jwt-token-auth-success");
    assertEquals(expectedAuthSuccess, actualAuthSuccess);
    long actualAuthFailure =
        (long) metrics.getMetric("impala.thrift-server.hiveserver2-http-frontend."
            + "total-jwt-token-auth-failure");
    assertEquals(expectedAuthFailure, actualAuthFailure);
  }

  /**
   * Tests if sessions are authenticated by verifying the JWT token for connections
   * to the HTTP hiveserver2 endpoint.
   * Since we don't have Java version of JWT library, we use pre-calculated JWT token
   * and JWKS. The token and JWK set used in this test case were generated by using
   * BE unit-test function JwtUtilTest::VerifyJwtRS256.
   */
  @Test
  public void testJwtAuth() throws Exception {
    String jwksFilename =
        new File(System.getenv("IMPALA_HOME"), "testdata/jwt/jwks_rs256.json").getPath();
    setUp(String.format(
        "--jwt_token_auth=true --jwt_validate_signature=true --jwks_file_path=%s "
            + "--jwt_allow_without_tls=true",
        jwksFilename));
    THttpClient transport = new THttpClient("http://localhost:28000");
    Map<String, String> headers = new HashMap<String, String>();

    // Case 1: Authenticate with valid JWT Token in HTTP header.
    String jwtToken =
        "eyJhbGciOiJSUzI1NiIsImtpZCI6InB1YmxpYzpjNDI0YjY3Yi1mZTI4LTQ1ZDctYjAxNS1m"
        + "NzlkYTUwYjViMjEiLCJ0eXAiOiJKV1MifQ.eyJpc3MiOiJhdXRoMCIsInVzZXJuYW1lIjoia"
        + "W1wYWxhIn0.OW5H2SClLlsotsCarTHYEbqlbRh43LFwOyo9WubpNTwE7hTuJDsnFoVrvHiWI"
        + "02W69TZNat7DYcC86A_ogLMfNXagHjlMFJaRnvG5Ekag8NRuZNJmHVqfX-qr6x7_8mpOdU55"
        + "4kc200pqbpYLhhuK4Qf7oT7y9mOrtNrUKGDCZ0Q2y_mizlbY6SMg4RWqSz0RQwJbRgXIWSgc"
        + "bZd0GbD_MQQ8x7WRE4nluU-5Fl4N2Wo8T9fNTuxALPiuVeIczO25b5n4fryfKasSgaZfmk0C"
        + "oOJzqbtmQxqiK9QNSJAiH2kaqMwLNgAdgn8fbd-lB1RAEGeyPH8Px8ipqcKsPk0bg";
    headers.put("Authorization", "Bearer " + jwtToken);
    headers.put("X-Forwarded-For", "127.0.0.1");
    transport.setCustomHeaders(headers);
    transport.open();
    TCLIService.Iface client = new TCLIService.Client(new TBinaryProtocol(transport));

    // Open a session which will get username 'impala' from JWT token and use it as
    // login user.
    TOpenSessionReq openReq = new TOpenSessionReq();
    TOpenSessionResp openResp = client.OpenSession(openReq);
    // One successful authentication.
    verifyJwtAuthMetrics(1, 0);
    // Running a query should succeed.
    TOperationHandle operationHandle = execAndFetch(
        client, openResp.getSessionHandle(), "select logged_in_user()", "impala");
    // Two more successful authentications - for the Exec() and the Fetch().
    verifyJwtAuthMetrics(3, 0);

    // case 2: Authenticate fails with invalid JWT token which does not have signature.
    String invalidJwtToken =
        "eyJhbGciOiJSUzI1NiIsImtpZCI6InB1YmxpYzpjNDI0YjY3Yi1mZTI4LTQ1ZDctYjAxNS1m"
        + "NzlkYTUwYjViMjEiLCJ0eXAiOiJKV1MifQ.eyJpc3MiOiJhdXRoMCIsInVzZXJuYW1lIjoia"
        + "W1wYWxhIn0.";
    headers.put("Authorization", "Bearer " + invalidJwtToken);
    headers.put("X-Forwarded-For", "127.0.0.1");
    transport.setCustomHeaders(headers);
    try {
      openResp = client.OpenSession(openReq);
      fail("Exception exception.");
    } catch (Exception e) {
      verifyJwtAuthMetrics(3, 1);
      assertEquals(e.getMessage(), "HTTP Response code: 401");
    }

    // case 3: Authenticate fails without "Bearer" token.
    headers.put("Authorization", "Basic VGVzdDFMZGFwOjEyMzQ1");
    headers.put("X-Forwarded-For", "127.0.0.1");
    transport.setCustomHeaders(headers);
    try {
      openResp = client.OpenSession(openReq);
      fail("Exception exception.");
    } catch (Exception e) {
      // JWT authentication is not invoked.
      verifyJwtAuthMetrics(3, 1);
      assertEquals(e.getMessage(), "HTTP Response code: 401");
    }

    // case 4: Authenticate fails without "Authorization" header.
    headers.put("X-Forwarded-For", "127.0.0.1");
    transport.setCustomHeaders(headers);
    try {
      openResp = client.OpenSession(openReq);
      fail("Exception exception.");
    } catch (Exception e) {
      // JWT authentication is not invoked.
      verifyJwtAuthMetrics(3, 1);
      assertEquals(e.getMessage(), "HTTP Response code: 401");
    }
  }

  /**
   * Tests if sessions are authenticated by verifying the JWT token for connections
   * to the HTTP hiveserver2 endpoint.
   */
  @Test
  public void testJwtAuthNotVerifySig() throws Exception {
    // Start Impala without jwt_validate_signature as false so that the signature of
    // JWT token will not be validated.
    setUp("--jwt_token_auth=true --jwt_validate_signature=false "
        + "--jwt_allow_without_tls=true");
    THttpClient transport = new THttpClient("http://localhost:28000");
    Map<String, String> headers = new HashMap<String, String>();

    // Case 1: Authenticate with valid JWT Token in HTTP header.
    // The Token was generated by BE unit-test function JwtUtilTest::VerifyJwtRS256.
    String jwtToken =
        "eyJhbGciOiJSUzI1NiIsImtpZCI6InB1YmxpYzpjNDI0YjY3Yi1mZTI4LTQ1ZDctYjAxNS1m"
        + "NzlkYTUwYjViMjEiLCJ0eXAiOiJKV1MifQ.eyJpc3MiOiJhdXRoMCIsInVzZXJuYW1lIjoia"
        + "W1wYWxhIn0.OW5H2SClLlsotsCarTHYEbqlbRh43LFwOyo9WubpNTwE7hTuJDsnFoVrvHiWI"
        + "02W69TZNat7DYcC86A_ogLMfNXagHjlMFJaRnvG5Ekag8NRuZNJmHVqfX-qr6x7_8mpOdU55"
        + "4kc200pqbpYLhhuK4Qf7oT7y9mOrtNrUKGDCZ0Q2y_mizlbY6SMg4RWqSz0RQwJbRgXIWSgc"
        + "bZd0GbD_MQQ8x7WRE4nluU-5Fl4N2Wo8T9fNTuxALPiuVeIczO25b5n4fryfKasSgaZfmk0C"
        + "oOJzqbtmQxqiK9QNSJAiH2kaqMwLNgAdgn8fbd-lB1RAEGeyPH8Px8ipqcKsPk0bg";
    headers.put("Authorization", "Bearer " + jwtToken);
    headers.put("X-Forwarded-For", "127.0.0.1");
    transport.setCustomHeaders(headers);
    transport.open();
    TCLIService.Iface client = new TCLIService.Client(new TBinaryProtocol(transport));

    // Open a session which will get username 'impala' from JWT token.
    TOpenSessionReq openReq = new TOpenSessionReq();
    TOpenSessionResp openResp = client.OpenSession(openReq);
    // One successful authentication.
    verifyJwtAuthMetrics(1, 0);
    // Running a query should succeed.
    TOperationHandle operationHandle = execAndFetch(
        client, openResp.getSessionHandle(), "select logged_in_user()", "impala");
    // Two more successful authentications - for the Exec() and the Fetch().
    verifyJwtAuthMetrics(3, 0);
  }
}
