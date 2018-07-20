/************************************************************************************
    Copyright (C) 2018 Georg Richter and MariaDB Corporation AB

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; either
   version 2 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not see <http://www.gnu.org/licenses>
   or write to the Free Software Foundation, Inc.,
   51 Franklin St., Fifth Floor, Boston, MA 02110, USA
*************************************************************************************/
#include "mariadb_python.h"
#include <datetime.h>

MYSQL_LEX_STRING pickle_key= {"pickle", 6};

/* {{{ mariadb_pydate_to_tm
   converts a Python date/time/datetime object to MYSQL_TIME
*/
static void mariadb_pydate_to_tm(enum enum_field_types type,
                                    PyObject *obj,
                                    MYSQL_TIME *tm)
{
  if (!PyDateTimeAPI)
    PyDateTime_IMPORT;
  memset(tm, 0, sizeof(MYSQL_TIME));
  if (type == MYSQL_TYPE_TIME ||
      type == MYSQL_TYPE_DATETIME)
  {
    uint8_t is_time= PyTime_CheckExact(obj);
    tm->hour= is_time ? PyDateTime_TIME_GET_HOUR(obj) :
                        PyDateTime_DATE_GET_HOUR(obj);
    tm->minute= is_time ? PyDateTime_TIME_GET_MINUTE(obj) :
                          PyDateTime_DATE_GET_MINUTE(obj);
    tm->second= is_time ? PyDateTime_TIME_GET_SECOND(obj) :
                          PyDateTime_DATE_GET_SECOND(obj);
    tm->second_part= is_time ? PyDateTime_TIME_GET_MICROSECOND(obj) :
                               PyDateTime_DATE_GET_MICROSECOND(obj);
    if (type == MYSQL_TYPE_TIME)
    {
      tm->time_type= MYSQL_TIMESTAMP_TIME;
      return;
    }
  }
  if (type == MYSQL_TYPE_DATE ||
      type == MYSQL_TYPE_DATETIME)
  {
    tm->year= PyDateTime_GET_YEAR(obj);
    tm->month= PyDateTime_GET_MONTH(obj);
    tm->day= PyDateTime_GET_DAY(obj);
    if (type == MYSQL_TYPE_DATE)
      tm->time_type= MYSQL_TIMESTAMP_DATE;
    else
      tm->time_type= MYSQL_TIMESTAMP_DATETIME;
  }
}
/* }}} */

/* {{{ check_is_dyncol

   Checks if the binary object is a dynamic column with the following
   conditions:
   - dyncol has names and is not stored in old format
   - number of elements (columns) can't be zero
   - first value is an integer
   - 4 high bytes of integer = PYTHON_DYNCOL_VALUE
 */
static uint8_t check_is_dyncol(DYNAMIC_COLUMN *col, DYNAMIC_COLUMN_VALUE *val)
{
  uint32_t count= 0;

  if (mariadb_dyncol_check(col) != ER_DYNCOL_OK ||
      !mariadb_dyncol_has_names(col))
    return 0;

  if (mariadb_dyncol_column_count(col, &count) != ER_DYNCOL_OK ||
      count != 1)
    return 0;

  if (mariadb_dyncol_get_named(col, &pickle_key, val) != ER_DYNCOL_OK)
    return 0;

  if (val->type != DYN_COL_STRING)
    return 0;

  return 1;
}
/* }}} */

/* {{{ field_fetch_callback
  This function was previously registered with mysql_stmt_attr_set and
  STMT_ATTR_FIELD_FETCH_CALLBACK parameter. Instead of filling a bind buffer
  MariaDB Connector/C sends raw data in row for the specified column. In case
  of a NULL value row ptr will be NULL.

  The cursor handle was also previously registered with mysql_stmt_attr_set
  and STMT_ATTR_USER_DATA parameter and will be passed in data variable.
*/

void field_fetch_callback(void *data, unsigned int column, unsigned char **row)
{
  MrdbCursor *self= (MrdbCursor *)data;

  if (!PyDateTimeAPI)
    PyDateTime_IMPORT;

  if (!row)
  {
    Py_INCREF(Py_None);
    self->values[column]= Py_None;
    return;
  }
  switch(self->fields[column].type) {
    case MYSQL_TYPE_NULL:
      Py_INCREF(Py_None);
      self->values[column]= Py_None;
      break;
    case MYSQL_TYPE_TINY:
      self->values[column]= (self->fields[column].flags & UNSIGNED_FLAG) ?
           PyLong_FromUnsignedLong((unsigned long)*row[0]) :
           PyLong_FromLong((long)*row[0]);
      *row+= 1;
      break;
    case MYSQL_TYPE_SHORT:
    case MYSQL_TYPE_YEAR:
      self->values[column]= (self->fields[column].flags & UNSIGNED_FLAG) ?
           PyLong_FromUnsignedLong((unsigned long)uint2korr(*row)) :
           PyLong_FromLong((long)sint2korr(*row));
      *row+= 2;
      break;
    case MYSQL_TYPE_INT24:
      self->values[column]= (self->fields[column].flags & UNSIGNED_FLAG) ?
           PyLong_FromUnsignedLong((unsigned long)uint3korr(*row)) :
           PyLong_FromLong((long)sint3korr(*row));
      *row+= 4;
      break;
    case MYSQL_TYPE_LONG:
      self->values[column]= (self->fields[column].flags & UNSIGNED_FLAG) ?
           PyLong_FromUnsignedLong((unsigned long)uint4korr(*row)) :
           PyLong_FromLong((long)sint4korr(*row));
      *row+= 4;
      break;
    case MYSQL_TYPE_LONGLONG:
    {
      long long l= sint8korr(*row);
      self->values[column]= (self->fields[column].flags & UNSIGNED_FLAG) ?
           PyLong_FromUnsignedLongLong((unsigned long long)l) :
           PyLong_FromLong(l);
      *row+= 8;
      break;
    }
    case MYSQL_TYPE_FLOAT:
    {
      float f;
      float4get(f, *row);
      self->values[column]= PyFloat_FromDouble((double)f);
      *row+= 4;
      break;
    }
    case MYSQL_TYPE_DOUBLE:
    {
      double d;
      float8get(d, *row);
      self->values[column]= PyFloat_FromDouble(d);
      *row+= 8;
      break;
    }
    case MYSQL_TYPE_DATETIME:
    case MYSQL_TYPE_TIMESTAMP:
    {
      uint8_t len= 0;
      int year= 0, month= 0, day= 0,
          hour= 0, minute= 0, second= 0, second_part= 0;

      len= (uint8_t)mysql_net_field_length(row);
      if (!len)
      {
        self->values[column]= PyDateTime_FromDateAndTime(0,0,0,0,0,0,0);
        break;
      }
      year= uint2korr(*row);
      month= uint1korr(*row + 2);
      day= uint1korr(*row + 3);
      if (len > 4)
      {
        hour= uint1korr(*row + 4);
        minute= uint1korr(*row + 5);
        second= uint1korr(*row + 6);
      }
      if (len == 11)
        second_part= uint4korr(*row + 7);
      self->values[column]= PyDateTime_FromDateAndTime(year, month, day, hour, minute, second, second_part);
      *row+= len;
      break;
    }
    case MYSQL_TYPE_DATE:
    {
      uint8_t len= 0;
      int year, month, day;

      len= (uint8_t)mysql_net_field_length(row);

      if (!len)
      {
        self->values[column]= PyDate_FromDate(0,0,0);
        break;
      }
      year= uint2korr(*row);
      month= uint1korr(*row + 2);
      day= uint1korr(*row + 3);
      self->values[column]= PyDate_FromDate(year, month, day);
      *row+= len;
      break;
    }
    case MYSQL_TYPE_TIME:
    {
      uint8_t len= 0,
              is_negative= 0,
              minute= 0,
              second= 0;
      int32_t hour;
      uint32_t day, second_part= 0;

      len= (uint8_t)mysql_net_field_length(row);
      if (!len)
      {
        self->values[column]= PyTime_FromTime(0,0,0,0);
        break;
      }
      is_negative= uint1korr(*row);
      day= uint4korr(*row + 1);
      hour= uint1korr(*row + 5);
      minute= uint1korr(*row + 6);
      second= uint1korr(*row + 7);
      if (len > 8)
        second_part= uint4korr(*row + 8);
      if (day)
        hour+= (day * 24);
      if (is_negative)
        hour*= -1;
      self->values[column]= PyTime_FromTime(hour, minute, second, second_part);
      *row+= len;
      break;
    }
    case MYSQL_TYPE_TINY_BLOB:
    case MYSQL_TYPE_MEDIUM_BLOB:
    case MYSQL_TYPE_BLOB:
    case MYSQL_TYPE_LONG_BLOB:
    {
      unsigned long length= mysql_net_field_length(row);
      if (self->fields[column].flags & BINARY_FLAG)
      {
        DYNAMIC_COLUMN dc;
        DYNAMIC_COLUMN_VALUE val;
        uint8_t rc;

        /* check if we have a DYNAMIC_COLUMN */
        mariadb_dyncol_init(&dc);
        dc.str= (char *)*row;
        dc.length= length;

        if ((rc= check_is_dyncol(&dc, &val)))
        {
          /* We got a serialized object which is stored in a dynamic column */
          PyObject *byte= PyBytes_FromStringAndSize(val.x.string.value.str,
                                                    val.x.string.value.length);
          self->values[column]= PyObject_CallMethod(Mrdb_Pickle, "loads", "O", byte);
          Py_DECREF(byte);
        }
        else
          self->values[column]= PyBytes_FromStringAndSize((const char *)*row, (Py_ssize_t)length);
      }
      else
        self->values[column]= PyUnicode_FromStringAndSize((const char *)*row, (Py_ssize_t)length);
 
      *row+= length;
      break;
    }
    case MYSQL_TYPE_GEOMETRY:
    case MYSQL_TYPE_STRING:
    case MYSQL_TYPE_VAR_STRING:
    case MYSQL_TYPE_JSON:
    case MYSQL_TYPE_VARCHAR:
    case MYSQL_TYPE_DECIMAL:
    case MYSQL_TYPE_NEWDECIMAL:
    case MYSQL_TYPE_SET:
    case MYSQL_TYPE_ENUM:
    {
      unsigned long length;
      length= mysql_net_field_length(row);
      self->values[column]= PyUnicode_FromStringAndSize((const char *)*row, (Py_ssize_t)length);
      *row+= length;
    }
    default:
      break;
  }
}
/* }}} */

/* {{{ mariadb_get_column_info
   This function analyzes the Python object and calculates the corresponding
   MYSQL_TYPE, unsigned flag or NULL values and stores the information in
   MrdbParamInfo pointer.
*/
static uint8_t mariadb_get_column_info(PyObject *obj,
                                MrdbParamInfo *paraminfo)
{
  if (!PyDateTimeAPI)
    PyDateTime_IMPORT;

  if (obj == NULL)
  {
    paraminfo->type= MYSQL_TYPE_BLOB;
    return 0;
  }

  if (Py_TYPE(obj) == &PyLong_Type)
  {
    size_t b= _PyLong_NumBits(obj);
    if (b > paraminfo->bits)
      paraminfo->bits= b;
    if (_PyLong_Sign(obj) < 0)
      paraminfo->is_negative= 1;
    paraminfo->type= MYSQL_TYPE_LONGLONG;
    return 0;
  } else if (Py_TYPE(obj) == &PyFloat_Type)
  {
    paraminfo->type= MYSQL_TYPE_DOUBLE;
    return 0;
  } else if (Py_TYPE(obj) == &PyBytes_Type)
  {
    paraminfo->type= MYSQL_TYPE_LONG_BLOB;
    return 0;
  } else if (PyDate_CheckExact(obj))
  {
    paraminfo->type= MYSQL_TYPE_DATE;
    return 0;
  } else if (PyTime_CheckExact(obj))
  {
    paraminfo->type= MYSQL_TYPE_TIME;
    return 0;
  } else if (PyDateTime_CheckExact(obj))
  {
    paraminfo->type= MYSQL_TYPE_DATETIME;
    return 0;
  } else if (Py_TYPE(obj) == &PyUnicode_Type)
  {
    paraminfo->type= MYSQL_TYPE_VAR_STRING;
    return 0;
  } else if (obj == Py_None)
  {
    paraminfo->type= MYSQL_TYPE_NULL;
    return 0;
  }
  else {
    /* no corresponding object, so we will serialize it */
    paraminfo->type= MYSQL_TYPE_LONG_BLOB;
    paraminfo->ob_type= Py_TYPE(obj);
    return 0;
  }

  return 1;
}
/* }}} */

/* {{{ mariadb_get_parameter 

  mariadb_get_parameter()
  @brief   Returns a bulk parameter which was passed to
           cursor.executemany() or a parameter which was
           passed to cursor.execute()

  @param   self[in]         Cursor
  @param   row_nr[in]       row number
  @paran   column_nr[in]    column number
  @param   paran[in][out]   bulk parameter pointer

  @return  0 on success, 1 on error

*/
static uint8_t mariadb_get_parameter(MrdbCursor *self,
                                     uint8_t is_bulk,
                                     uint32_t row_nr,
                                     uint32_t column_nr,
                                     MrdbParamValue *param)
{
  PyObject *row= NULL,
           *column= NULL;

  if (is_bulk)
  {
    /* check if row_nr and column_nr are in the range from
       0 to (value - 1) */
    if (row_nr > (self->array_size - 1) ||
        column_nr > (self->param_count - 1) ||
        row_nr < 0 ||
        column_nr < 0)
    {
      mariadb_throw_exception(self->stmt, Mariadb_DataError, 0,
      "Can't access data at row %d, column %d", row_nr + 1, column_nr + 1);
      return 1;
    }

    if (!(row= PyList_GetItem(self->data, row_nr)))
    {
      mariadb_throw_exception(self->stmt, Mariadb_DataError, 0,
      "Can't access row number %d", row_nr + 1);
      return 1;
    }
  }
  else
    row= self->data;

  if (!(column= PyTuple_GetItem(row, column_nr)))
  {
    mariadb_throw_exception(self->stmt, Mariadb_DataError, 0,
    "Can't access column number %d at row %d", column_nr + 1, row_nr + 1);
    return 1;
  }

  /* check if an indicator was passed */
  if (MrdbIndicator_Check(column))
  {
    if (!MARIADB_FEATURE_SUPPORTED(self->stmt->mysql, 100206))
    {
      mariadb_throw_exception(NULL, Mariadb_DataError, 0,
      "MariaDB %s doesn't support indicator variables. Required version is 10.2.6 or newer", mysql_get_server_info(self->stmt->mysql));
      return 1;
    }
    param->indicator= MrdbIndicator_AsLong(column);
    param->value= NULL; /* you can't have both indicator and value */
  } else if (column == Py_None)
  {
    if (MARIADB_FEATURE_SUPPORTED(self->stmt->mysql, 100206))
    {
      param->indicator= STMT_INDICATOR_NULL;
      param->value= NULL;
    }
  }  else
  {
    param->value= column;
    param->indicator= STMT_INDICATOR_NONE;
  }
  return 0;
}
/* }}} */

/* {{{ mariadb_get_parameter_info
   mariadb_get_parameter_info fills the MYSQL_BIND structure
   with correct field_types for the Python objects.

   In case of a bulk operation (executemany()) we will also optimize
   the field type (e.g. by checking maxbit size for a PyLong).
   If the types in this column differ we will return an error.
*/
static uint8_t mariadb_get_parameter_info(MrdbCursor *self,
                                   MYSQL_BIND *param,
                                   uint32_t column_nr)
{
  uint32_t i, bits= 0;
  MrdbParamValue paramvalue;
  MrdbParamInfo pinfo;

  /* Assume unsigned */
  param->is_unsigned= 1;

  if (!self->array_size)
  {
    memset(&pinfo, 0, sizeof(MrdbParamInfo));
    if (mariadb_get_parameter(self, 0, 0, column_nr, &paramvalue))
      return 1;
    if (mariadb_get_column_info(paramvalue.value, &pinfo))
    {
      mariadb_throw_exception(NULL, Mariadb_DataError, 0,
                              "Can't retrieve column information for parameter %d",
                              column_nr);
      return 1;
    }
    param->buffer_type= pinfo.type;
    bits= pinfo.bits;
  }

  for (i=0; i < self->array_size; i++)
  {
    if (mariadb_get_parameter(self, 1, i, column_nr, &paramvalue))
      return 1;
    memset(&pinfo, 0, sizeof(MrdbParamInfo));
    if (mariadb_get_column_info(paramvalue.value, &pinfo))
    {
      mariadb_throw_exception(NULL, Mariadb_DataError, 1,
      "Invalid parameter type at row %d, column %d", i+1, column_nr + 1);
      return 1;
    }

    if (pinfo.is_negative)
      param->is_unsigned= 0;

    if (pinfo.type == MYSQL_TYPE_LONGLONG)
    {
      if (pinfo.bits > bits)
        bits= pinfo.bits;
    }


    if (!param->buffer_type ||
         param->buffer_type == MYSQL_TYPE_NULL)
      param->buffer_type= pinfo.type;
    else {
      /* except for NULL the parameter types must match */
      if (param->buffer_type != pinfo.type &&
          pinfo.type != MYSQL_TYPE_NULL)
      {
        if ((param->buffer_type == MYSQL_TYPE_TINY ||
             param->buffer_type == MYSQL_TYPE_SHORT ||
             param->buffer_type == MYSQL_TYPE_LONG) &&
             pinfo.type == MYSQL_TYPE_LONGLONG)
          break;
        mariadb_throw_exception(NULL, Mariadb_DataError, 1,
        "Invalid parameter type at row %d, column %d", i+1, column_nr + 1);
        return 1;
      }
    }
  }
  /* check the bit size for long types and set the appropiate
     field type */
  if (param->buffer_type == MYSQL_TYPE_LONGLONG)
  {
    if (bits <= 8)
      param->buffer_type= MYSQL_TYPE_TINY;
    else if (bits <= 16)
      param->buffer_type= MYSQL_TYPE_SHORT;
    else if (bits <= 32)
      param->buffer_type= MYSQL_TYPE_LONG;
    else if (bits <= 64)
      param->buffer_type= MYSQL_TYPE_LONGLONG;
    else
      param->buffer_type= MYSQL_TYPE_VAR_STRING;
  }
  return 0;
}
/* }}} */

/* {{{ mariadb_check_bulk_parameters
   This function validates the specified bulk parameters and
   translates the field types to MYSQL_TYPE_*.
*/
uint8_t mariadb_check_bulk_parameters(MrdbCursor *self,
                                      PyObject *data)
{
  uint32_t i;

  if (!(self->array_size= PyList_Size(data)))
  {
    mariadb_throw_exception(self->stmt, Mariadb_InterfaceError, 1, 
    "Empty parameter list. At least one row must be specified");
    return 1;
  }

  for (i=0; i < self->array_size; i++)
  {
    PyObject *obj= PyList_GetItem(data, i);
    if (Py_TYPE(obj) != &PyTuple_Type)
    {
      mariadb_throw_exception(NULL, Mariadb_DataError, 0,
      "Invalid parameter type in row %d. (Row data must be provided as tuple(s))", i+1);
      return 1;
    }

    if (!self->param_count && !self->is_prepared)
      self->param_count= PyTuple_Size(obj);

    if (!self->param_count ||
        self->param_count != PyTuple_Size(obj))
    {
      mariadb_throw_exception(self->stmt, Mariadb_DataError, 1, 
      "Invalid number of parameters in row %d", i+1);
      return 1;
    }
  }

  if (!self->is_prepared &&
      !(self->params= PyMem_RawCalloc(self->param_count, sizeof(MYSQL_BIND))))
  {
    mariadb_throw_exception(NULL, Mariadb_InterfaceError, 0,
                            "Not enough memory (tried to allocated %lld bytes)",
                            self->param_count * sizeof(MYSQL_BIND));
    goto error;
  }

  if (!(self->value= PyMem_RawCalloc(self->param_count, sizeof(MrdbParamValue))))
  {
    mariadb_throw_exception(NULL, Mariadb_InterfaceError, 0,
                            "Not enough memory (tried to allocated %lld bytes)",
                            self->param_count * sizeof(MrdbParamValue));
    goto error;
  }

  for (i=0; i < self->param_count; i++)
  {
    if (mariadb_get_parameter_info(self, &self->params[i], i))
      goto error;
  }
  return 0;
error:
  MARIADB_FREE_MEM(self->paraminfo);
  MARIADB_FREE_MEM(self->value);
  return 1;
}
/* }}} */

/* {{{ mariadb_check_execute_parameters */
uint8_t mariadb_check_execute_parameters(MrdbCursor *self,
                                         PyObject *data)
{
  uint32_t i;
  if (!self->is_prepared)
    self->param_count= PyTuple_Size(data);

  if (!self->param_count)
  {
    mariadb_throw_exception(NULL, Mariadb_DataError, 0,
    "Invalid number of parameters");
    return 1;
  }

  if (!self->is_prepared &&
      !(self->params= PyMem_RawCalloc(self->param_count, sizeof(MYSQL_BIND))))
  {
    mariadb_throw_exception(NULL, Mariadb_InterfaceError, 0,
                            "Not enough memory (tried to allocated %lld bytes)",
                            self->param_count * sizeof(MYSQL_BIND));
    goto error;
  }

  if (!(self->value= PyMem_RawCalloc(self->param_count, sizeof(MrdbParamValue))))
  {
    mariadb_throw_exception(NULL, Mariadb_InterfaceError, 0,
                            "Not enough memory (tried to allocated %lld bytes)",
                            self->param_count * sizeof(MrdbParamValue));
    goto error;
  }

  for (i=0; i < self->param_count; i++)
  {
    if (mariadb_get_parameter_info(self, &self->params[i], i))
      goto error;
  }
  return 0;
error:
  MARIADB_FREE_MEM(self->paraminfo);
  MARIADB_FREE_MEM(self->value);
  return 1;
}
/* }}} */

/* {{{ mariadb_param_to_bind */
/**
  mariadb_param_to_bind()

  @brief Set the current value for the specified bind buffer

  @param bind[in]   bind structure
  @param value[in]  current column value

  @return 0 on succes, otherwise error
*/
static uint8_t mariadb_param_to_bind(MYSQL_BIND *bind,
                                     MrdbParamValue *value)
{
  if (value->indicator > 0)
  {
    bind->u.indicator[0]= value->indicator;
    return 0;
  }

  if (IS_NUM(bind->buffer_type))
    bind->buffer= value->num;

  switch(bind->buffer_type)
  {
    case MYSQL_TYPE_TINY:
      if (bind->is_unsigned)
        value->num[0]= (uint8_t)PyLong_AsUnsignedLong(value->value);
      else
        value->num[0]= (int8_t)PyLong_AsLong(value->value);
      break;
    case MYSQL_TYPE_SHORT:
      if (bind->is_unsigned)
        *(uint16_t *)&value->num= (uint16_t)PyLong_AsUnsignedLong(value->value);
      else
        *(int16_t *)&value->num= (int16_t)PyLong_AsLong(value->value);
      break;
    case MYSQL_TYPE_LONG:
      if (bind->is_unsigned)
        *(uint32_t *)&value->num= (uint32_t)PyLong_AsUnsignedLong(value->value);
      else
        *(int32_t *)&value->num= (int32_t)PyLong_AsLong(value->value);
      break;
    case MYSQL_TYPE_LONGLONG:
      if (bind->is_unsigned)
        *(uint64_t *)value->num= (uint64_t)PyLong_AsUnsignedLongLong(value->value);
      else
        *(int64_t *)value->num= (int64_t)PyLong_AsLongLong(value->value);
      break;
    case MYSQL_TYPE_DOUBLE:
      *(double *)value->num= (double)PyFloat_AsDouble(value->value);
      break;
    case MYSQL_TYPE_LONG_BLOB:
      if (Py_TYPE(value->value) != &PyBytes_Type)
      {
        DYNAMIC_COLUMN_VALUE dynval;
        PyObject *dump= NULL;
        if (value->dyncol.length)
          mariadb_dyncol_free(&value->dyncol);
        dump= PyObject_CallMethod(Mrdb_Pickle, "dumps", "O", value->value);
        mariadb_dyncol_init(&value->dyncol);
        dynval.type= DYN_COL_STRING;
        PyBytes_AsStringAndSize(dump, &dynval.x.string.value.str,
                                (Py_ssize_t *)&dynval.x.string.value.length);
        mariadb_dyncol_create_many_named(&value->dyncol, 1, &pickle_key,
                                         &dynval, 0);
        Py_DECREF(dump);
        bind->buffer_length= (unsigned long)value->dyncol.length;
        bind->buffer= (void *)value->dyncol.str;
      } else
      {
        bind->buffer_length= (unsigned long)PyBytes_GET_SIZE(value->value);
        bind->buffer= (void *) PyBytes_AS_STRING(value->value);
      }
      break;
    case MYSQL_TYPE_DATE:
    case MYSQL_TYPE_TIME:
    case MYSQL_TYPE_DATETIME:
      bind->buffer= &value->tm;
      mariadb_pydate_to_tm(bind->buffer_type, value->value, &value->tm);
      break;
    case MYSQL_TYPE_VAR_STRING:
    {
      Py_ssize_t len;
      bind->buffer= (void *)PyUnicode_AsUTF8AndSize(value->value, &len);
      bind->buffer_length= (unsigned long)len;
      break;
    }
    case MYSQL_TYPE_NULL:
      break;
    default:
      return 1;
  }
  return 0;
}
/* }}} */

/* {{{  mariadb_param_update */
/**
  mariadb_param_update()
  @brief   Callback function which updates the bind structure's buffer and
           length with data from the specified row number. This callback function
           must be registered via api function mysql_stmt_attr_set
           with STMT_ATTR_PARAM_CALLBACK option

  @param   data[in]      A pointer to a MrdbCursor object which was passed
                         via mysql_stmt_attr_set before
           data[in][out] An array of bind structures
           data[in]      row number

  @return  0 on success, otherwise error (=1)
*/
uint8_t mariadb_param_update(void *data, MYSQL_BIND *bind, uint32_t row_nr)
{
  MrdbCursor *self= (MrdbCursor *)data;
  uint32_t i;

  if (!self)
    return 1;

  for (i=0; i < self->param_count; i++)
  {
    if (mariadb_get_parameter(self, (self->array_size > 0), row_nr, i, &self->value[i]))
      return 1;
    if (self->value[i].indicator)
    {
      bind[i].u.indicator= &self->value[i].indicator;
    }
    if (self->value[i].indicator < 1)
    {
      if (mariadb_param_to_bind(&bind[i], &self->value[i]))
      {
        return 1;
      }
    }
  }
  return 0;
}
/* }}} */
