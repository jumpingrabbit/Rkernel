//  Rkernel is an execution kernel for R interpreter
//  Copyright (C) 2019 JetBrains s.r.o.
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <https://www.gnu.org/licenses/>.


#include "RPIServiceImpl.h"
#include <Rcpp.h>
#include <grpcpp/server_builder.h>
#include "RObjects.h"
#include "IO.h"
#include "util/RUtil.h"
#include "util/ContainerUtil.h"

const int MAX_PREVIEW_STRING_LENGTH = 200;
const int MAX_PREVIEW_PRINTED_COUNT = 20;

void getValueInfo(SEXP var, ValueInfo* result) {
  auto type = TYPEOF(var);
  if (type == PROMSXP) {
    if (PRVALUE(var) == R_UnboundValue) {
      std::string code = translateToUTF8(
        RI->paste(RI->deparse(RI->expression(PRCODE(var))), Rcpp::Named("collapse", " ")));
      const char* exprStr = "expression(";
      int exprLen = strlen(exprStr);
      if (!strncmp(exprStr, code.c_str(), exprLen) && code.back() == ')') {
        code = code.substr(exprLen, code.size() - 1 - exprLen);
      }
      result->mutable_unevaluated()->set_code(code);
      return;
    }
    getValueInfo(PRVALUE(var), result);
  } else if (type == CLOSXP || type == SPECIALSXP || type == BUILTINSXP) {
    result->mutable_function()->set_header(getFunctionHeader(var));
  } else if (type == ENVSXP) {
    result->mutable_environment()->set_name(translateToUTF8(RI->environmentName(var)));
  } else {
    Rcpp::CharacterVector classes = RI->classes(var);
    if (contains(classes, "ggplot")) {
      result->mutable_graph();
    } else if (contains(classes, "data.frame")) {
      ValueInfo::DataFrame* dataFrame = result->mutable_dataframe();
      dataFrame->set_rows(Rcpp::as<int>(RI->nrow(var)));
      dataFrame->set_cols(Rcpp::as<int>(RI->ncol(var)));
    } else if (type == VECSXP || type == LISTSXP) {
      result->mutable_list()->set_length(Rcpp::as<int>(RI->length(var)));
    } else {
      ValueInfo::Value* value = result->mutable_value();
      if (type == LGLSXP || type == INTSXP || type == REALSXP || type == CPLXSXP || type == NILSXP) {
        int length = Rcpp::as<int>(RI->length(var));
        value->set_isvector(length > 1);
        if (length <= MAX_PREVIEW_PRINTED_COUNT) {
          value->set_textvalue(getPrintedValue(RI->unclass(var)));
          value->set_iscomplete(true);
        } else {
          value->set_textvalue(getPrintedValue(RI->unclass(
              RI->subscript(var, RI->colon(1, MAX_PREVIEW_PRINTED_COUNT)))));
          value->set_iscomplete(false);
        }
      } else if (type == STRSXP) {
        int length = Rcpp::as<int>(RI->length(var));
        value->set_isvector(length > 1);
        bool isComplete = length <= MAX_PREVIEW_PRINTED_COUNT;
        Rcpp::CharacterVector vector = isComplete ? var : RI->subscript(var, RI->colon(1, MAX_PREVIEW_PRINTED_COUNT));
        vector = RI->substring(vector, 1, MAX_PREVIEW_STRING_LENGTH);
        Rcpp::IntegerVector nchar = RI->nchar(vector);
        for (int i = 0; isComplete && i < (int)vector.size(); ++i) {
          if (!Rcpp::CharacterVector::is_na(vector[i]) && nchar[i] == MAX_PREVIEW_STRING_LENGTH) {
            isComplete = false;
          }
        }
        value->set_textvalue(getPrintedValue(RI->unclass(vector)));
        value->set_iscomplete(isComplete);
      } else {
        value->set_isvector(false);
        value->set_textvalue("");
        value->set_iscomplete(true);
      }
    }
  }
}

Status RPIServiceImpl::loaderGetParentEnvs(ServerContext* context, const RRef* request, ParentEnvsResponse* response) {
  executeOnMainThread([&] {
    Rcpp::Environment environment = Rcpp::as<Rcpp::Environment>(dereference(*request));
    while (environment != Rcpp::Environment::empty_env()) {
      environment = environment.parent();
      ParentEnvsResponse::EnvInfo* envInfo = response->add_envs();
      envInfo->set_name(translateToUTF8(RI->environmentName(environment)));
    }
  }, context);
  return Status::OK;
}

Status RPIServiceImpl::loaderGetVariables(ServerContext* context, const GetVariablesRequest* request, VariablesResponse* response) {
  executeOnMainThread([&] {
    Rcpp::RObject obj = dereference(request->obj());
    int reqStart = request->start();
    int reqEnd = request->end();
    if (reqEnd == -1) reqEnd = INT_MAX;
    if (Rcpp::as<bool>(RI->isEnvironment(obj))) {
      response->set_isenv(true);
      Rcpp::Environment environment = Rcpp::as<Rcpp::Environment>(obj);
      Rcpp::CharacterVector ls = Rcpp::as<Rcpp::CharacterVector>(environment.ls(true));
      response->set_totalcount(ls.size());
      for (int i = std::max(0, reqStart); i < std::min<int>(ls.size(), reqEnd); ++i) {
        const char* name = ls[i];
        VariablesResponse::Variable* var = response->add_vars();
        var->set_name(ls[i]);
        try {
          getValueInfo(Rf_findVar(Rf_install(name), environment), var->mutable_value());
        } catch (Rcpp::eval_error const& e) {
          var->mutable_value()->mutable_error()->set_text(e.what());
        }
      }
    } else {
      response->set_isenv(false);
      int length = Rcpp::as<int>(RI->length(obj));
      response->set_totalcount(length);
      Rcpp::RObject namesObj = RI->names(obj);
      Rcpp::CharacterVector names = namesObj == R_NilValue ? Rcpp::CharacterVector() : Rcpp::as<Rcpp::CharacterVector>(namesObj);
      for (int i = std::max(0, reqStart); i < std::min(length, reqEnd); ++i) {
        VariablesResponse::Variable* var = response->add_vars();
        var->set_name(i < names.size() && !Rcpp::CharacterVector::is_na(names[i]) ? names[i] : "");
        try {
          getValueInfo(RI->doubleSubscript(obj, i + 1), var->mutable_value());
        } catch (Rcpp::eval_error const& e) {
          var->mutable_value()->mutable_error()->set_text(e.what());
        }
      }
    }
  }, context);
  return Status::OK;
}

Status RPIServiceImpl::loaderGetLoadedNamespaces(ServerContext* context, const Empty*, StringList* response) {
  executeOnMainThread([&] {
    Rcpp::CharacterVector namespaces = RI->loadedNamespaces();
    for (const char* s : namespaces) {
      response->add_list(s);
    }
  }, context);
  return Status::OK;
}

Status RPIServiceImpl::loaderGetValueInfo(ServerContext* context, const RRef* request, ValueInfo* response) {
  executeOnMainThread([&] {
    try {
      Rcpp::RObject value = dereference(*request);
      getValueInfo(value, response);
    } catch (Rcpp::eval_error const& e) {
      response->mutable_error()->set_text(e.what());
    }
  }, context);
  return Status::OK;
}
