#include <mongoc.h>
#include "mongoc-cursor-private.h"
#include "mongoc-client-private.h"

#include "TestSuite.h"
#include "test-conveniences.h"
#include "mock_server/mock-server.h"
#include "mock_server/future.h"
#include "mock_server/future-functions.h"


typedef struct
{
   const char           *filter;
   bson_t               *filter_bson;
   const char           *opts;
   bson_t               *opts_bson;
   mongoc_read_prefs_t  *read_prefs;
   const char           *expected_find_command;
   const char           *expected_op_query;
   const char           *expected_op_query_projection;
   int32_t               expected_n_return;
   mongoc_query_flags_t  expected_flags;
   uint32_t              expected_skip;
} test_collection_find_with_opts_t;


typedef request_t *(*check_request_fn_t) (
   mock_server_t *server,
   test_collection_find_with_opts_t *test_data);


/*--------------------------------------------------------------------------
 *
 * _test_collection_op_query_or_find_command --
 *
 *       Start a mock server with @max_wire_version, connect a client, and
 *       execute a query with @test_data->filter and @test_data->opts. Use
 *       the @check_request_fn callback to verify the client formatted the
 *       query correctly, and @reply_json to respond to the client.
 *
 *--------------------------------------------------------------------------
 */

static void
_test_collection_op_query_or_find_command (
   test_collection_find_with_opts_t *test_data,
   check_request_fn_t                check_request_fn,
   const char                       *reply_json,
   int32_t                           max_wire_version)
{
   mock_server_t *server;
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   bson_error_t error;
   future_t *future;
   request_t *request;
   const bson_t *doc;

   server = mock_server_with_autoismaster (max_wire_version);
   mock_server_run (server);
   client = mongoc_client_new_from_uri (mock_server_get_uri (server));
   collection = mongoc_client_get_collection (client, "db", "collection");
   cursor = mongoc_collection_find_with_opts (collection,
                                              test_data->filter_bson,
                                              test_data->read_prefs,
                                              test_data->opts_bson);

   ASSERT_OR_PRINT (!mongoc_cursor_error (cursor, &error), error);
   future = future_cursor_next (cursor, &doc);
   request = check_request_fn (server, test_data);
   ASSERT (request);
   mock_server_replies_simple (request, reply_json);
   ASSERT (future_get_bool (future));

   request_destroy (request);
   future_destroy (future);
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
   mock_server_destroy (server);
}


static request_t *
_check_op_query (mock_server_t                    *server,
                 test_collection_find_with_opts_t *test_data)
{
   mongoc_query_flags_t flags;
   request_t *request;
   const bson_t *doc;
   bson_iter_t iter;
   uint32_t len;
   const uint8_t *data;
   bson_t query;

   /* Server Selection Spec: all queries to standalone set slaveOk. */
   flags = test_data->expected_flags | MONGOC_QUERY_SLAVE_OK;

   request = mock_server_receives_query (
      server,
      "db.collection",
      flags,
      test_data->expected_skip,
      test_data->expected_n_return,
      test_data->expected_op_query,
      test_data->expected_op_query_projection);

   ASSERT (request);

   /* check that nothing unexpected is in $query */
   if (bson_empty (test_data->filter_bson)) {
      doc = request_get_doc (request, 0);

      if (bson_iter_init_find (&iter, doc, "$query")) {
         ASSERT (BSON_ITER_HOLDS_DOCUMENT (&iter));
         bson_iter_document (&iter, &len, &data);
         bson_init_static (&query, data, (size_t) len);
         ASSERT (bson_empty (&query));
      }
   }

   return request;
}


static void
_test_collection_op_query (test_collection_find_with_opts_t *test_data)
{
   _test_collection_op_query_or_find_command (test_data,
                                              _check_op_query,
                                              "{}",
                                              3 /* wire version */);
}


static request_t *
_check_find_command (mock_server_t                    *server,
                     test_collection_find_with_opts_t *test_data)
{
   /* Server Selection Spec: all queries to standalone set slaveOk.
    *
    * Find, getMore And killCursors Commands Spec: "When sending a find command
    * rather than a legacy OP_QUERY find only the slaveOk flag is honored".
    */
   return mock_server_receives_command (server, "db", MONGOC_QUERY_SLAVE_OK,
                                        test_data->expected_find_command);
}


static void
_test_collection_find_command (test_collection_find_with_opts_t *test_data)

{
   _test_collection_op_query_or_find_command (test_data,
                                              _check_find_command,
                                              "{'ok': 1,"
                                              " 'cursor': {"
                                              "    'id': 0,"
                                              "    'ns': 'db.collection',"
                                              "    'firstBatch': [{}]}}",
                                              4 /* max wire version */);
}


static void
_test_collection_find_with_opts (test_collection_find_with_opts_t *test_data)
{
   BSON_ASSERT (test_data->expected_find_command);

   test_data->filter_bson = tmp_bson (test_data->filter);
   test_data->opts_bson = tmp_bson (test_data->opts);

   _test_collection_op_query (test_data);
   _test_collection_find_command (test_data);
}


static void
test_dollar_or (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.filter = "{'$or': [{'_id': 1}]}";
   test_data.expected_op_query = "{'$query': {'$or': [{'_id': 1}]}}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {'$or': [{'_id': 1}]}}";

   _test_collection_find_with_opts (&test_data);
}


/* test that we can query for a document by a key named "filter" */
static void
test_key_named_filter (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.filter = "{'filter': 2}";
   test_data.expected_op_query = "{'$query': {'filter': 2}}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {'filter': 2}}";
   _test_collection_find_with_opts (&test_data);
}


/* test 'filter': {'filter': {'i': 2}} */
static void
test_op_query_subdoc_named_filter (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.filter = "{'filter': {'i': 2}}";
   test_data.expected_op_query = "{'$query': {'filter': {'i': 2}}}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {'filter': {'i': 2}}}";
   _test_collection_find_with_opts (&test_data);
}


/* test 'filter': {'filter': {'i': 2}}, 'singleBatch': true
 * we just use singleBatch to prove that an option can be passed
 * alongside 'filter'
 */
static void
test_find_cmd_subdoc_named_filter_with_option (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.filter = "{'filter': {'i': 2}}";
   test_data.opts = "{'singleBatch': true}";
   test_data.expected_op_query = "{'$query': {'filter': {'i': 2}}}";
   test_data.expected_n_return = -1;
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {'filter': {'i': 2}}, "
      " 'singleBatch': true}";

   _test_collection_find_with_opts (&test_data);
}

/* this can't work until later in CDRIVER-1522 implementation */
#ifdef TODO_CDRIVER_1522
/* test future-compatibility with a new server's find command options */
static void
test_newoption (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.filter = "{'_id': 1}";
   test_data.opts = "{'newOption': true}";
   test_data.expected_op_query = "{'$query': {'_id': 1}, 'newOption': true}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {'_id': 1}, 'newOption': true}";

   _test_collection_find (&test_data);
}
#endif


static void
test_sort (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'sort': {'_id': -1}}";
   test_data.expected_op_query = "{'$query': {}, '$orderby': {'_id': -1}}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'sort': {'_id': -1}}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_fields (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'projection': {'_id': 0, 'b': 1}}";
   test_data.expected_op_query_projection = "{'_id': 0, 'b': 1}}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'projection': {'_id': 0, 'b': 1}}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_int_modifiers (void)
{
   const char *modifiers[] = {
      "maxScan",
      "maxTimeMS",
   };

   const char *mod;
   size_t i;
   char *opts;
   char *query;
   char *find_command;
   test_collection_find_with_opts_t test_data = { 0 };

   for (i = 0; i < sizeof (modifiers) / sizeof (const char *); i++) {
      mod = modifiers[i];
      opts = bson_strdup_printf ("{'%s': {'$numberLong': '9999'}}", mod);
      query = bson_strdup_printf ("{'$query': {},"
                                  " '$%s': {'$numberLong': '9999'}}", mod);

      /* find command has same modifier, without the $-prefix */
      find_command = bson_strdup_printf (
         "{'find': 'collection', 'filter': {},"
         " '%s': {'$numberLong': '9999'}}", mod);

      test_data.opts = opts;
      test_data.expected_op_query = query;
      test_data.expected_find_command = find_command;
      _test_collection_find_with_opts (&test_data);

      bson_free (opts);
      bson_free (query);
      bson_free (find_command);
   }
}


static void
test_index_spec_modifiers (void)
{
   const char *modifiers[] = {
      "hint",
      "min",
      "max",
   };

   const char *mod;
   size_t i;
   char *opts;
   char *query;
   char *find_command;
   test_collection_find_with_opts_t test_data = { 0 };

   for (i = 0; i < sizeof (modifiers) / sizeof (const char *); i++) {
      mod = modifiers[i];
      opts = bson_strdup_printf ("{'%s': {'_id': 1}}", mod);
      /* OP_QUERY modifiers use $-prefix: $hint, $min, $max */
      query = bson_strdup_printf ("{'$query': {}, '$%s': {'_id': 1}}", mod);

      /* find command options have no $-prefix: hint, min, max */
      find_command = bson_strdup_printf (
         "{'find': 'collection', 'filter': {}, '%s': {'_id': 1}}", mod);

      test_data.opts = opts;
      test_data.expected_op_query = query;
      test_data.expected_find_command = find_command;
      _test_collection_find_with_opts (&test_data);

      bson_free (opts);
      bson_free (query);
      bson_free (find_command);
   }
}


static void
test_comment (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'comment': 'hi'}";
   test_data.expected_op_query = "{'$query': {}, '$comment': 'hi'}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'comment': 'hi'}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_snapshot (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'snapshot': true}";
   test_data.expected_op_query = "{'$query': {}, '$snapshot': true}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'snapshot': true}";
   _test_collection_find_with_opts (&test_data);
}


/* showRecordId becomes $showDiskLoc */
static void
test_diskloc (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'showRecordId': true}";
   test_data.expected_op_query = "{'$query': {}, '$showDiskLoc': true}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'showRecordId': true}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_returnkey (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'returnKey': true}";
   test_data.expected_op_query = "{'$query': {}, '$returnKey': true}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'returnKey': true}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_skip (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.expected_skip = 1;
   test_data.opts = "{'skip': {'$numberLong': '1'}}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'skip': {'$numberLong': '1'}}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_batch_size (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'batchSize': {'$numberLong': '2'}}";
   test_data.expected_n_return = 2;
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'batchSize': {'$numberLong': '2'}}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_limit (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'limit': {'$numberLong': '2'}}";
   test_data.expected_n_return = 2;
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, 'limit': {'$numberLong': '2'}}";
   _test_collection_find_with_opts (&test_data);
}


static void
test_negative_limit (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'limit': {'$numberLong': '-2'}}";
   test_data.expected_n_return = -2;
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, "
      " 'singleBatch': true, 'limit': {'$numberLong': '2'}}";

   _test_collection_find_with_opts (&test_data);
}


static void
test_unrecognized_dollar_option (void)
{
   test_collection_find_with_opts_t test_data = { 0 };

   test_data.opts = "{'$dumb': 1}";
   test_data.expected_op_query = "{'$query': {}, '$dumb': 1}";
   test_data.expected_find_command =
      "{'find': 'collection', 'filter': {}, '$dumb': 1}";

   _test_collection_find_with_opts (&test_data);
}


static void
test_query_flags (void)
{
   int i;
   char *opts;
   char *find_cmd;
   test_collection_find_with_opts_t test_data = { 0 };

   typedef struct
   {
      mongoc_query_flags_t flag;
      const char          *name;
   } flag_and_name_t;

   /* slaveok is still in the wire protocol header, exhaust is not supported */
   flag_and_name_t flags_and_names[] = {
      { MONGOC_QUERY_TAILABLE_CURSOR,   "tailable"                      },
      { MONGOC_QUERY_OPLOG_REPLAY,      "oplogReplay"                   },
      { MONGOC_QUERY_NO_CURSOR_TIMEOUT, "noCursorTimeout"               },
      { MONGOC_QUERY_AWAIT_DATA,        "awaitData"                     },
      { MONGOC_QUERY_PARTIAL,           "allowPartialResults"           },
   };

   for (i = 0; i < (sizeof flags_and_names) / (sizeof (flag_and_name_t)); i++) {
      opts = bson_strdup_printf ("{'%s': true}", flags_and_names[i].name);
      find_cmd = bson_strdup_printf (
         "{'find': 'collection', 'filter': {}, '%s': true}",
         flags_and_names[i].name);

      test_data.opts = opts;
      test_data.expected_flags = flags_and_names[i].flag;
      test_data.expected_find_command = find_cmd;

      _test_collection_find_with_opts (&test_data);

      bson_free (find_cmd);
   }
}


static void
test_exhaust (void)
{
   mock_server_t *server;
   mongoc_client_t *client;
   mongoc_collection_t *collection;
   mongoc_cursor_t *cursor;
   request_t *request;
   future_t *future;
   const bson_t *doc;
   bson_error_t error;

   server = mock_server_with_autoismaster (WIRE_VERSION_FIND_CMD);
   mock_server_run (server);
   client = mongoc_client_new_from_uri (mock_server_get_uri (server));
   collection = mongoc_client_get_collection (client, "db", "collection");
   cursor = mongoc_collection_find_with_opts (collection,
                                              tmp_bson (NULL),
                                              NULL,
                                              tmp_bson ("{'exhaust': true}"));

   future = future_cursor_next (cursor, &doc);

   /* Find, getMore and killCursors commands spec: "The find command does not
    * support the exhaust flag from OP_QUERY. Drivers that support exhaust MUST
    * fallback to existing OP_QUERY wire protocol messages."
    */
   request = mock_server_receives_request (server);
   mock_server_replies_to_find (
      request, MONGOC_QUERY_SLAVE_OK | MONGOC_QUERY_EXHAUST,
      0, 0, "db.collection", "{}", false /* is_command */);

   ASSERT (future_get_bool (future));
   ASSERT_OR_PRINT (!mongoc_cursor_error (cursor, &error), error);

   request_destroy (request);
   future_destroy (future);
   mongoc_cursor_destroy (cursor);
   mongoc_collection_destroy (collection);
   mongoc_client_destroy (client);
   mock_server_destroy (server);
}


void
test_collection_find_with_opts_install (TestSuite *suite)
{
   TestSuite_Add (suite, "/Collection/find_with_opts/dollar_or",
                  test_dollar_or);
   TestSuite_Add (suite, "/Collection/find_with_opts/key_named_filter",
                  test_key_named_filter);
   TestSuite_Add (suite,
                  "/Collection/find_with_opts/query/subdoc_named_filter",
                  test_op_query_subdoc_named_filter);
/* this can't work until later in CDRIVER-1522 implementation */
#ifdef TODO_CDRIVER_1522
   TestSuite_Add (suite, "/Collection/find_with_opts/newoption",
                  test_newoption);
#endif
   TestSuite_Add (suite, "/Collection/find_with_opts/cmd/subdoc_named_filter",
                  test_find_cmd_subdoc_named_filter_with_option);
   TestSuite_Add (suite, "/Collection/find_with_opts/orderby",
                  test_sort);
   TestSuite_Add (suite, "/Collection/find_with_opts/fields",
                  test_fields);
   TestSuite_Add (suite, "/Collection/find_with_opts/modifiers/integer",
                  test_int_modifiers);
   TestSuite_Add (suite, "/Collection/find_with_opts/modifiers/index_spec",
                  test_index_spec_modifiers);
   TestSuite_Add (suite, "/Collection/find_with_opts/comment",
                  test_comment);
   TestSuite_Add (suite, "/Collection/find_with_opts/modifiers/bool",
                  test_snapshot);
   TestSuite_Add (suite, "/Collection/find_with_opts/showdiskloc",
                  test_diskloc);
   TestSuite_Add (suite, "/Collection/find_with_opts/returnkey",
                  test_returnkey);
   TestSuite_Add (suite, "/Collection/find_with_opts/skip",
                  test_skip);
   TestSuite_Add (suite, "/Collection/find_with_opts/batch_size",
                  test_batch_size);
   TestSuite_Add (suite, "/Collection/find_with_opts/limit",
                  test_limit);
   TestSuite_Add (suite, "/Collection/find_with_opts/negative_limit",
                  test_negative_limit);
   TestSuite_Add (suite, "/Collection/find_with_opts/unrecognized_dollar",
                  test_unrecognized_dollar_option);
   TestSuite_Add (suite, "/Collection/find_with_opts/flags",
                  test_query_flags);
   TestSuite_Add (suite, "/Collection/find_with_opts/exhaust",
                  test_exhaust);
}
