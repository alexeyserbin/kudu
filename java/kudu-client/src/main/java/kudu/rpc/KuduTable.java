// Copyright (c) 2013, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.
package kudu.rpc;

import kudu.Schema;
import kudu.util.Slice;

import java.util.List;

/**
 * A KuduTable represents a table on a particular cluster. It holds the current
 * schema of the table. Any given KuduTable instance belongs to a specific KuduClient
 * instance.
 *
 * Upon construction, the table is looked up in the catalog (or catalog cache),
 * and the schema fetched for introspection.
 *
 * This class is thread-safe.
 */
public class KuduTable {

  private final Schema schema;
  private final KuduClient client;
  private final String name;
  private final Slice nameAsSlice;

  KuduTable(KuduClient client, String name, Schema schema) {
    this.schema = schema;
    this.client = client;
    this.name = name;
    this.nameAsSlice = new Slice(this.name.getBytes());
  }

  public Schema getSchema() {
    return this.schema;
  }

  public String getName() {
    return this.name;
  }

  public KuduClient getClient() {
    return this.client;
  }

  public Slice getNameAsSlice() {
    return this.nameAsSlice;
  }

  public Insert newInsert() {
    return new Insert(this);
  }

  public Update newUpdate() {
    return new Update(this);
  }

  public Delete newDelete() {
    return new Delete(this);
  }

  /**
   * Get all the tablets for this table. This may query the master multiple times if there
   * are a lot of tablets.
   * @param deadline deadline in milliseconds for this method to finish
   * @return a list containing the metadata and locations for each of the tablets in the
   *         table
   * @throws Exception
   */
  public List<LocatedTablet> getTabletsLocations(
      long deadline) throws Exception {
    return getTabletsLocations(null, null, deadline);
  }

  /**
   * Get all or some tablets for this table. This may query the master multiple times if there
   * are a lot of tablets.
   * This method blocks until it gets all the tablets.
   * @param startKey where to start in the table, pass null to start at the beginning
   * @param endKey where to stop in the table, pass null to get all the tablets until the end of
   *               the table
   * @param deadline deadline in milliseconds for this method to finish
   * @return a list containing the metadata and locations for each of the tablets in the
   *         table
   * @throws Exception
   */
  public List<LocatedTablet> getTabletsLocations(
      byte[] startKey, byte[] endKey, long deadline) throws Exception {
    return client.syncLocateTable(name, startKey, endKey, deadline);
  }

}
